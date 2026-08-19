using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DeskosAgent;

/// <summary>
/// Persisted agent settings — mirrors the device's /sd/agent.json convention
/// (docs/host-protocol.md, ROADMAP #12): a shared token, nothing fancier.
/// Generated on first run so the user only has to copy one value onto the
/// microSD card, not invent one.
/// </summary>
public sealed class AgentConfig
{
    public string Token { get; set; } = "";
    public int Port { get; set; } = 7332;

    [JsonIgnore]
    public string FilePath { get; private set; } = "";

    public static AgentConfig LoadOrCreate()
    {
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "DeskosAgent");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "config.json");

        AgentConfig config;
        if (File.Exists(path))
        {
            var text = File.ReadAllText(path);
            config = JsonSerializer.Deserialize<AgentConfig>(text) ?? new AgentConfig();
        }
        else
        {
            config = new AgentConfig();
        }

        // A missing/empty token means "never configured" (fresh install) —
        // same convention the device side uses for /sd/agent.json.
        if (string.IsNullOrEmpty(config.Token))
        {
            config.Token = Convert.ToHexString(RandomNumberGenerator.GetBytes(8)).ToLowerInvariant();
        }

        config.FilePath = path;
        config.Save();
        return config;
    }

    public void Save()
    {
        var text = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(FilePath, text);
    }
}
