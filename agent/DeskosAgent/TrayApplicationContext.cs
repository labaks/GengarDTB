namespace DeskosAgent;

/// <summary>
/// No main window on purpose — this is a background service with a tray
/// icon, not a desktop app. ApplicationContext (rather than a hidden Form)
/// is the standard WinForms shape for that.
/// </summary>
public sealed class TrayApplicationContext : ApplicationContext
{
    private readonly NotifyIcon _icon;
    private readonly AgentConfig _config;
    private readonly DeviceServer _server;
    private readonly ToolStripMenuItem _autostartItem;
    private readonly ToolStripMenuItem _statusItem;

    // A hidden window purely to marshal DeviceServer's log callbacks (which
    // land on arbitrary thread-pool threads) onto the UI thread. NotifyIcon
    // is a Component, not a Control, so it has no Invoke of its own — and
    // SynchronizationContext.Current isn't set up yet this early: Application.Run
    // is what installs the WinForms one, and that happens strictly after this
    // constructor returns. Forcing Handle creation here makes Invoke usable
    // immediately; queued calls are simply processed once Run starts pumping.
    private readonly Control _uiThread = new();

    public TrayApplicationContext()
    {
        _ = _uiThread.Handle;

        _config = AgentConfig.LoadOrCreate();
        _server = new DeviceServer(_config, new CpuMonitor());
        _server.Log += OnServerLog;

        var menu = new ContextMenuStrip();

        _statusItem = new ToolStripMenuItem("Waiting for a device...") { Enabled = false };
        menu.Items.Add(_statusItem);
        menu.Items.Add(new ToolStripSeparator());

        menu.Items.Add(new ToolStripMenuItem("Copy pairing token", null, (_, _) =>
        {
            Clipboard.SetText(_config.Token);
        }));
        menu.Items.Add(new ToolStripMenuItem("Open config folder", null, (_, _) =>
        {
            System.Diagnostics.Process.Start("explorer.exe",
                $"/select,\"{_config.FilePath}\"");
        }));

        var autostartItem = new ToolStripMenuItem("Start with Windows") { Checked = AutoStart.IsEnabled() };
        autostartItem.Click += (_, _) =>
        {
            AutoStart.SetEnabled(!AutoStart.IsEnabled());
            autostartItem.Checked = AutoStart.IsEnabled();
        };
        _autostartItem = autostartItem;
        menu.Items.Add(_autostartItem);

        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(new ToolStripMenuItem("Exit", null, (_, _) => ExitThread()));

        _icon = new NotifyIcon
        {
            Icon = System.Drawing.SystemIcons.Application,
            Text = "deskos agent",
            ContextMenuStrip = menu,
            Visible = true,
        };
        _icon.BalloonTipTitle = "deskos agent";
        _icon.BalloonTipText =
            $"Pairing token: {_config.Token}\nPut it in /sd/agent.json on the device's microSD card.";
        _icon.ShowBalloonTip(8000);

        ThreadExit += (_, _) => Shutdown();
        _server.Start();
    }

    private void OnServerLog(string message)
    {
        // No-op when launched normally (a WinExe app has no console to write
        // to), but visible when run from a terminal for debugging.
        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {message}");
        if (_uiThread.IsHandleCreated)
        {
            _uiThread.BeginInvoke(() => _statusItem.Text = message);
        }
    }

    private void Shutdown()
    {
        _server.Dispose();
        _icon.Visible = false;
        _icon.Dispose();
        _uiThread.Dispose();
    }
}
