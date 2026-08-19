using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Makaretu.Dns;

namespace DeskosAgent;

/// <summary>
/// Device-facing half of docs/host-protocol.md: mDNS advertisement plus the
/// WebSocket server the ESP32 connects out to (device is the client, agent
/// is the server — see the protocol doc for why).
///
/// Deliberately not HttpListener: binding it to a wildcard prefix
/// ("http://+:port/...") needs either admin rights or a one-time elevated
/// "netsh http add urlacl", which is a bad first-run experience for a
/// background tray app. A bare TcpListener plus a hand-rolled WebSocket
/// upgrade (RFC 6455) needs neither — WebSocket.CreateFromStream() (added in
/// .NET 5) does the framing once the handshake is done by hand.
/// </summary>
public sealed class DeviceServer : IDisposable
{
    private const string WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    private const string ServiceType = "_deskos-agent._tcp";
    private static readonly string[] SupportedTopics = ["cpu"];
    private static readonly TimeSpan PushInterval = TimeSpan.FromSeconds(2);

    private readonly AgentConfig _config;
    private readonly CpuMonitor _cpu;
    private readonly TcpListener _listener;
    private readonly ServiceDiscovery _discovery;
    private readonly CancellationTokenSource _cts = new();

    public event Action<string>? Log;

    public DeviceServer(AgentConfig config, CpuMonitor cpu)
    {
        _config = config;
        _cpu = cpu;
        _listener = new TcpListener(IPAddress.Any, config.Port);
        _discovery = new ServiceDiscovery();
    }

    public void Start()
    {
        _listener.Start();

        var profile = new ServiceProfile("deskos-agent", ServiceType, (ushort)_config.Port);
        profile.AddProperty("v", "1");
        _discovery.Advertise(profile);

        Log?.Invoke($"listening on port {_config.Port}, mDNS service {ServiceType}.local advertised");
        _ = AcceptLoopAsync(_cts.Token);
    }

    public void Dispose()
    {
        _cts.Cancel();
        _listener.Stop();
        _discovery.Unadvertise();
        _discovery.Dispose();
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _listener.AcceptTcpClientAsync(ct);
            }
            catch (Exception) when (ct.IsCancellationRequested)
            {
                return;
            }
            _ = HandleTcpClientAsync(client, ct);
        }
    }

    private async Task HandleTcpClientAsync(TcpClient client, CancellationToken ct)
    {
        using var _ = client;
        client.NoDelay = true;
        var stream = client.GetStream();

        string? key;
        try
        {
            key = await ReadWebSocketKeyAsync(stream, ct);
        }
        catch (Exception ex)
        {
            Log?.Invoke($"handshake failed: {ex.Message}");
            return;
        }
        if (key == null)
        {
            Log?.Invoke("connection dropped: not a WebSocket upgrade");
            return;
        }

        var accept = Convert.ToBase64String(SHA1.HashData(Encoding.ASCII.GetBytes(key + WebSocketGuid)));
        var response =
            "HTTP/1.1 101 Switching Protocols\r\n" +
            "Upgrade: websocket\r\n" +
            "Connection: Upgrade\r\n" +
            $"Sec-WebSocket-Accept: {accept}\r\n\r\n";
        await stream.WriteAsync(Encoding.ASCII.GetBytes(response), ct);

        using var socket = WebSocket.CreateFromStream(stream, isServer: true, subProtocol: null,
            keepAliveInterval: TimeSpan.FromSeconds(30));
        var remote = client.Client.RemoteEndPoint;
        Log?.Invoke($"device connected from {remote}");
        await HandleDeviceAsync(socket, ct);
        Log?.Invoke($"device {remote} disconnected");
    }

    /// <summary>
    /// Reads the HTTP upgrade request line-by-line just far enough to pull
    /// Sec-WebSocket-Key, then stops. Assumes the whole request arrives
    /// before any WebSocket frame does — true for esp_websocket_client (and
    /// every normal client), which waits for the 101 response before sending
    /// anything else.
    /// </summary>
    private static async Task<string?> ReadWebSocketKeyAsync(NetworkStream stream, CancellationToken ct)
    {
        var buffer = new byte[4096];
        var total = 0;
        while (total < buffer.Length)
        {
            var n = await stream.ReadAsync(buffer.AsMemory(total, buffer.Length - total), ct);
            if (n == 0)
            {
                return null;
            }
            total += n;
            var text = Encoding.ASCII.GetString(buffer, 0, total);
            var headerEnd = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
            if (headerEnd < 0)
            {
                continue;
            }
            foreach (var line in text[..headerEnd].Split("\r\n"))
            {
                var idx = line.IndexOf(':');
                if (idx > 0 && line[..idx].Equals("Sec-WebSocket-Key", StringComparison.OrdinalIgnoreCase))
                {
                    return line[(idx + 1)..].Trim();
                }
            }
            return null;
        }
        return null;
    }

    private async Task HandleDeviceAsync(WebSocket socket, CancellationToken serverCt)
    {
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(serverCt);
        var subscribed = new HashSet<string>();
        Task? pushTask = null;
        var buffer = new byte[4096];

        try
        {
            while (socket.State == WebSocketState.Open)
            {
                WebSocketReceiveResult result;
                try
                {
                    result = await socket.ReceiveAsync(buffer, linkedCts.Token);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    break;
                }

                JsonObject msg;
                try
                {
                    msg = JsonNode.Parse(buffer.AsSpan(0, result.Count))?.AsObject()
                          ?? throw new JsonException("not an object");
                }
                catch (JsonException)
                {
                    Log?.Invoke("malformed message from device, ignored");
                    continue;
                }

                var type = msg["type"]?.GetValue<string>();
                switch (type)
                {
                    case "hello":
                        if (!await HandleHelloAsync(socket, msg))
                        {
                            return; // error already sent; connection is done
                        }
                        pushTask = PushLoopAsync(socket, subscribed, linkedCts.Token);
                        break;

                    case "subscribe":
                        subscribed.Clear();
                        if (msg["topics"] is JsonArray topics)
                        {
                            foreach (var t in topics)
                            {
                                var name = t?.GetValue<string>();
                                if (name != null && SupportedTopics.Contains(name))
                                {
                                    subscribed.Add(name);
                                }
                            }
                        }
                        await SendAsync(socket, new { type = "subscribed", topics = subscribed.ToArray() });
                        break;
                }
            }
        }
        finally
        {
            linkedCts.Cancel();
            if (pushTask != null)
            {
                try { await pushTask; } catch { /* cancellation is expected */ }
            }
            if (socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
            {
                try
                {
                    await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None);
                }
                catch { /* best effort */ }
            }
        }
    }

    /// <returns>false if hello was rejected (an error was sent and the caller should stop).</returns>
    private async Task<bool> HandleHelloAsync(WebSocket socket, JsonObject msg)
    {
        var token = msg["token"]?.GetValue<string>();
        var proto = msg["proto"]?.GetValue<int>() ?? 0;

        if (token != _config.Token)
        {
            Log?.Invoke("rejected hello: bad token");
            await SendAsync(socket, new { type = "error", code = "unauthorized" });
            return false;
        }
        if (proto != 1)
        {
            Log?.Invoke($"rejected hello: unsupported proto {proto}");
            await SendAsync(socket, new { type = "error", code = "unsupported_proto", supported = new[] { 1 } });
            return false;
        }

        var deviceId = msg["device_id"]?.GetValue<string>() ?? "?";
        Log?.Invoke($"hello from device {deviceId}, proto {proto} — accepted");
        await SendAsync(socket, new
        {
            type = "hello_ack",
            proto = 1,
            agent = "deskos-agent-dotnet",
            version = AgentVersion.String,
            topics = SupportedTopics,
        });
        return true;
    }

    private async Task PushLoopAsync(WebSocket socket, HashSet<string> subscribed, CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested && socket.State == WebSocketState.Open)
            {
                await Task.Delay(PushInterval, ct);
                if (subscribed.Contains("cpu"))
                {
                    await SendAsync(socket, new
                    {
                        type = "data",
                        topic = "cpu",
                        ts = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        value = new { percent = _cpu.GetPercent(), temp_c = (int?)null },
                    });
                }
            }
        }
        catch (OperationCanceledException)
        {
            // normal on disconnect/shutdown
        }
    }

    private static async Task SendAsync(WebSocket socket, object payload)
    {
        var json = JsonSerializer.Serialize(payload);
        var bytes = Encoding.UTF8.GetBytes(json);
        await socket.SendAsync(bytes, WebSocketMessageType.Text, true, CancellationToken.None);
    }
}
