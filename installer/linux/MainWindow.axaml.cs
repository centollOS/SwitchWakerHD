using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Path = Avalonia.Controls.Shapes.Path;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace SwitchWakerHD.Installer;

public partial class MainWindow : Window
{
    enum Page { Welcome, Game, Progress, Done }

    readonly LinuxEngine engine = new();
    Page page = Page.Welcome;
    bool running, failed, closingConfirmed;
    CancellationTokenSource? cts;
    ManualResetEventSlim? noticeEvent;
    string? noticeCommand;

    // The log: lines queue up from the worker thread and reach the TextBox in batches; all of it also goes to a file.
    readonly ConcurrentQueue<string> logQueue = new();
    readonly DispatcherTimer logTimer = new() { Interval = TimeSpan.FromMilliseconds(200) };
    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch elapsed = new();
    readonly object logFileLock = new();
    StreamWriter? logFile;
    readonly StringBuilder logText = new();
    const int LogBoxMaxChars = 200_000;
    static readonly string LogPath = System.IO.Path.Combine(EngineBase.WorkDir, "installer.log");

    readonly string[] stepKeys = { "step_python", "step_devkit", "step_release", "step_build" };
    readonly (Ellipse ring, Path glyph, TextBlock label)[] stepRows = new (Ellipse, Path, TextBlock)[4];
    readonly StepState[] stepStates = new StepState[4];

    public MainWindow()
    {
        InitializeComponent();
        BuildStepRows();

        engine.Log         += (msg, kind) => EnqueueLog(msg, kind);
        engine.Status      += msg => Ui(() => TxtStatus.Text = msg);
        engine.Progress    += p => Ui(() => SetProgress(p));
        engine.StepChanged += (i, s) => Ui(() => SetStep(i, s));
        engine.AskUser      = AskUser;

        logTimer.Tick += (_, _) => FlushLog();
        logTimer.Start();
        clock.Tick += (_, _) => TxtElapsed.Text = Str.Get("elapsed", elapsed.Elapsed.ToString(@"hh\:mm\:ss"));

        Closing += Window_Closing;
        AddHandler(DragDrop.DropEvent, Window_Drop);

        OptWua.IsChecked = true;
        ShowPage(Page.Welcome);
        ApplyLanguage();
    }

    static void Ui(Action a) => Dispatcher.UIThread.Post(a);
    IBrush Res(string key) => (IBrush)this.FindResource(key)!;

    // ── Pages and texts ──────────────────────────────────────────────────────
    void ShowPage(Page p)
    {
        page = p;
        PageWelcome.IsVisible  = p == Page.Welcome;
        PageGame.IsVisible     = p == Page.Game;
        PageProgress.IsVisible = p == Page.Progress;
        PageDone.IsVisible     = p == Page.Done;
        BtnLang.IsVisible      = p == Page.Welcome || p == Page.Game;
        UpdateFooter();
    }

    void UpdateFooter()
    {
        void Set(Button b, string? key, bool enabled = true)
        {
            b.IsVisible = key != null;
            if (key != null) b.Content = Str.Get(key);
            b.IsEnabled = enabled;
        }
        switch (page)
        {
            case Page.Welcome:  Set(BtnSecondary, null);         Set(BtnPrimary, "btn_start"); break;
            case Page.Game:     Set(BtnSecondary, "btn_back");   Set(BtnPrimary, "btn_install", GameReady()); break;
            case Page.Progress:
                if (running) { Set(BtnSecondary, "btn_cancel"); Set(BtnPrimary, null); }
                else         { Set(BtnSecondary, "btn_close");  Set(BtnPrimary, failed ? "btn_retry" : null); }
                break;
            case Page.Done:     Set(BtnSecondary, null);         Set(BtnPrimary, "btn_close", !xferBusy); break;
        }
    }

    void ApplyLanguage()
    {
        Title             = Str.Get("title");
        TxtSubtitle.Text  = Str.Get("subtitle");
        BtnLang.Content   = Str.Get("lang");

        TxtWelcomeH.Text     = Str.Get("welcome_h");
        TxtWelcomeBody.Text  = Str.Get("welcome_body");
        TxtWelcomeListH.Text = Str.Get("welcome_list_h");
        TxtWelcomeLi1.Text   = "•  " + Str.Get("lx_li1");
        TxtWelcomeLi2.Text   = "•  " + Str.Get("lx_li2");
        TxtWelcomeLi3.Text   = "•  " + Str.Get("welcome_li3");
        TxtWelcomeNeed.Text  = Str.Get("welcome_need");
        TxtWelcomeRoot.Text  = Str.Get("lx_root_note");
        TxtWelcomeDir.Text   = Str.Get("welcome_dir", EngineBase.WorkDir);

        TxtGameH.Text    = Str.Get("game_h");
        TxtGameBody.Text = Str.Get("game_body");
        TxtOptWua.Text   = Str.Get("game_opt_wua");  TxtOptWuaD.Text = Str.Get("game_opt_wua_d");
        TxtOptImg.Text   = Str.Get("game_opt_img");  TxtOptImgD.Text = Str.Get("game_opt_img_d");
        TxtOptDir.Text   = Str.Get("game_opt_dir");  TxtOptDirD.Text = Str.Get("game_opt_dir_d");
        TxtGamePath.Watermark = Str.Get("game_none_yet");
        BtnBrowse.Content = Str.Get("btn_browse");
        ValidateGame();

        TxtProgH.Text    = Str.Get("prog_h");
        TxtProgBody.Text = Str.Get("prog_body");
        for (int i = 0; i < stepRows.Length; i++) stepRows[i].label.Text = Str.Get(stepKeys[i]);
        TxtErrorH.Text   = Str.Get("fail_h");
        TxtLogPath.Text  = Str.Get("log_at", LogPath);
        BtnNoticeCopy.Content = Str.Get("btn_copy_cmd");
        UpdateLogButtons();

        TxtDoneH.Text     = Str.Get("done_h");
        TxtDoneBody.Text  = Str.Get("done_body");
        TxtXferMtp.Text = Str.Get("xfer_mtp");       TxtXferMtpD.Text = Str.Get("xfer_mtp_d_lx");
        TxtXferSd.Text  = Str.Get("xfer_sd");        TxtXferSdD.Text  = Str.Get("xfer_sd_d");
        TxtXferManual.Text = Str.Get("xfer_manual"); TxtXferManualD.Text = Str.Get("xfer_manual_d");
        TxtMtpSteps.Text  = Str.Get("mtp_steps");
        TxtMtpLabel.Text  = Str.Get("mtp_label");
        TxtMtpNone.Text   = Str.Get("mtp_none_lx");
        TxtMtpReplace.Text = Str.Get("mtp_replace_lx");
        BtnCopyMtp.Content = Str.Get("btn_copy_mtp");
        BtnMtpRefresh.Content = Str.Get("btn_refresh");
        TxtSdHint.Text    = Str.Get("sd_hint_lx");
        TxtSdLabel.Text   = Str.Get("sd_label");
        BtnSdRefresh.Content = Str.Get("btn_refresh");
        TxtSdNone.Text    = Str.Get("sd_none");
        BtnCopySd.Content = Str.Get("btn_copy_sd");
        TxtManual.Text    = Str.Get("manual_body");
        BtnOpenOut.Content = Str.Get("btn_open_out");
        BtnOpenOut.IsVisible = FindOnPath("xdg-open") != null;
        TxtNextH.Text = Str.Get("next_h");
        TxtNext1.Text = Str.Get(XferMtp.IsChecked == true ? "next_1_mtp" : "next_1"); TxtNext2.Text = Str.Get("next_2");
        TxtNext3.Text = Str.Get("next_3"); TxtNext4.Text = Str.Get("next_4");

        BtnConfirmYes.Content = Str.Get("btn_yes_cancel");
        BtnConfirmNo.Content  = Str.Get("btn_no");

        UpdateFooter();
    }

    void BtnLang_Click(object? sender, RoutedEventArgs e)
    {
        Str.Es = !Str.Es;
        ApplyLanguage();
    }

    void BtnPrimary_Click(object? sender, RoutedEventArgs e)
    {
        switch (page)
        {
            case Page.Welcome:  ShowPage(Page.Game); break;
            case Page.Game:     if (GameReady()) StartInstall(); break;
            case Page.Progress: if (!running && failed) StartInstall(); break;
            case Page.Done:     Close(); break;
        }
    }

    async void BtnSecondary_Click(object? sender, RoutedEventArgs e)
    {
        switch (page)
        {
            case Page.Game:     ShowPage(Page.Welcome); break;
            case Page.Progress:
                if (!running) { Close(); break; }
                if (await AskYesNo(Str.Get("cancel_q"))) CancelRun();
                break;
        }
    }

    // ── Yes/No over the window ───────────────────────────────────────────────
    TaskCompletionSource<bool>? confirmTcs;

    Task<bool> AskYesNo(string question)
    {
        TxtConfirm.Text = question;
        Confirm.IsVisible = true;
        confirmTcs = new TaskCompletionSource<bool>();
        BtnConfirmNo.Focus();
        return confirmTcs.Task;
    }

    void BtnConfirm_Click(object? sender, RoutedEventArgs e)
    {
        Confirm.IsVisible = false;
        confirmTcs?.TrySetResult(sender == BtnConfirmYes);
    }

    // ── Game page ────────────────────────────────────────────────────────────
    string gamePath = "";
    bool settingPath;

    void Opt_Changed(object? sender, RoutedEventArgs e)
    {
        if (sender is RadioButton { IsChecked: true }) ValidateGame();
    }

    void GamePath_Changed(object? sender, TextChangedEventArgs e)
    {
        if (settingPath) return;
        gamePath = (TxtGamePath.Text ?? "").Trim().Trim('"', '\'');
        if (gamePath.StartsWith("file://")) gamePath = Uri.UnescapeDataString(gamePath.Substring(7));
        if (gamePath.StartsWith("~/")) gamePath = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), gamePath.Substring(2));
        ValidateGame();
    }

    void SetGamePath(string path)
    {
        gamePath = path;
        settingPath = true;
        TxtGamePath.Text = path;
        settingPath = false;
        ValidateGame();
    }

    string GameArg() => OptImg.IsChecked == true ? "--image" : OptWua.IsChecked == true ? "--wua" : "--game-dir";

    bool PathFitsFormat(string path) => GameArg() == "--game-dir" ? Directory.Exists(path) : File.Exists(path);

    static bool IsGameDir(string d) =>
        new[] { "code", "content", "meta" }.All(s => Directory.Exists(System.IO.Path.Combine(d, s)));

    bool GameReady() =>
        gamePath != "" && PathFitsFormat(gamePath) && (GameArg() != "--game-dir" || IsGameDir(gamePath));

    void ValidateGame()
    {
        string? warn = null;
        if (gamePath != "" && !PathFitsFormat(gamePath)) warn = Str.Get("game_none");
        else if (gamePath != "" && GameArg() == "--game-dir" && !IsGameDir(gamePath)) warn = Str.Get("game_bad_dir");
        else if (gamePath != "" && GameArg() == "--image" &&
                 !File.Exists(System.IO.Path.ChangeExtension(gamePath, ".key")) &&
                 !File.Exists(System.IO.Path.Combine(System.IO.Path.GetDirectoryName(gamePath) ?? "", "GAME.key")))
            warn = Str.Get("game_no_keys");
        TxtGameWarn.Text = warn ?? "";
        TxtGameWarn.IsVisible = warn != null;
        if (page == Page.Game) UpdateFooter();
    }

    async void BtnBrowse_Click(object? sender, RoutedEventArgs e)
    {
        try
        {
            if (GameArg() == "--game-dir")
            {
                var r = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = Str.Get("dlg_folder") });
                var p = r.FirstOrDefault()?.TryGetLocalPath();
                if (p != null) SetGamePath(p);
            }
            else
            {
                var img = GameArg() == "--image";
                var type = img
                    ? new FilePickerFileType(Str.Get("game_opt_img")) { Patterns = new[] { "*.wux", "*.wud", "*.WUX", "*.WUD" } }
                    : new FilePickerFileType(Str.Get("game_opt_wua")) { Patterns = new[] { "*.wua", "*.WUA" } };
                var r = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
                {
                    Title = Str.Get(img ? "dlg_title_img" : "dlg_title_wua"),
                    FileTypeFilter = new[] { type, FilePickerFileTypes.All },
                });
                var p = r.FirstOrDefault()?.TryGetLocalPath();
                if (p != null) SetGamePath(p);
            }
        }
        catch (Exception ex)
        {
            // No portal and no GTK: the path box still takes a typed or pasted path.
            TxtGameWarn.Text = ex.Message;
            TxtGameWarn.IsVisible = true;
        }
    }

    // Dropping the game file or folder on the window picks it and its format.
    void Window_Drop(object? sender, DragEventArgs e)
    {
        if (running || page == Page.Done) return;
#pragma warning disable CS0618   // Data still works in Avalonia 11; DataTransfer is the 12 API
        var path = e.Data.GetFiles()?.FirstOrDefault()?.TryGetLocalPath();
#pragma warning restore CS0618
        if (path == null) return;
        var ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        if (Directory.Exists(path)) OptDir.IsChecked = true;
        else if (ext == ".wua") OptWua.IsChecked = true;
        else if (ext == ".wux" || ext == ".wud") OptImg.IsChecked = true;
        else return;
        if (page != Page.Game) ShowPage(Page.Game);
        SetGamePath(path);
    }

    // ── Install ──────────────────────────────────────────────────────────────
    async void StartInstall()
    {
        engine.GamePath = gamePath;
        engine.GameArg  = GameArg();
        running = true; failed = false;
        cts = new CancellationTokenSource();
        for (int i = 0; i < stepStates.Length; i++) SetStep(i, StepState.Pending);
        ErrorBox.IsVisible = false;
        Notice.IsVisible = false;
        TxtStatus.Text = "";
        OpenLogFile();
        ShowPage(Page.Progress);
        elapsed.Restart();
        clock.Start();

        string? error = null;
        try
        {
            var token = cts.Token;
            await Task.Run(() => engine.Run(token));
        }
        catch (InstallException ex)         { error = ex.Message; }
        catch (OperationCanceledException)  { error = Str.Get("cancelled"); }
        catch (Exception ex)                { error = ex.Message; EnqueueLog(ex.ToString(), LogKind.Error); }
        finally
        {
            elapsed.Stop();
            clock.Stop();
            running = false;
        }

        if (error == null)
        {
            EnqueueLog(Str.Get("done_h"), LogKind.Ok);
            FlushLog();
            EnterDone();
            return;
        }

        failed = true;
        EnqueueLog(error, LogKind.Error);
        TxtError.Text = error;
        ErrorBox.IsVisible = true;
        TxtStatus.Text = "";
        Bar.IsIndeterminate = false;
        Bar.Value = 0;
        for (int i = 0; i < stepStates.Length; i++)
            if (stepStates[i] == StepState.Running) SetStep(i, StepState.Failed);
        ShowLog(true);
        UpdateFooter();
    }

    void CancelRun()
    {
        BtnSecondary.IsEnabled = false;
        TxtStatus.Text = Str.Get("cancelling");
        cts?.Cancel();
        Task.Run(() => engine.KillCurrent());
    }

    async void Window_Closing(object? sender, WindowClosingEventArgs e)
    {
        if (running && !closingConfirmed)
        {
            e.Cancel = true;
            if (!await AskYesNo(Str.Get("cancel_q"))) return;
            closingConfirmed = true;
            cts?.Cancel();
            engine.KillCurrent();
            Close();
            return;
        }
        FlushLog();
        lock (logFileLock) { logFile?.Dispose(); logFile = null; }
    }

    // Called on the worker thread: shows the notice and waits for its button (or a cancel).
    void AskUser(string heading, string text, string button, string? command)
    {
        var ev = new ManualResetEventSlim();
        noticeEvent = ev;
        noticeCommand = command;
        Ui(() =>
        {
            TxtNoticeH.Text = heading;
            TxtNotice.Text = text;
            BtnNotice.Content = button;
            NoticeCmd.IsVisible = command != null;
            BtnNoticeCopy.IsVisible = command != null;
            TxtNoticeCmd.Text = command ?? "";
            Notice.IsVisible = true;
            Notice.BringIntoView();
            Activate();
        });
        try { ev.Wait(cts!.Token); }
        finally
        {
            noticeEvent = null;
            Ui(() => Notice.IsVisible = false);
        }
    }

    void BtnNotice_Click(object? sender, RoutedEventArgs e) => noticeEvent?.Set();

    async void BtnNoticeCopy_Click(object? sender, RoutedEventArgs e)
    {
        if (noticeCommand != null && Clipboard != null) await Clipboard.SetTextAsync(noticeCommand);
    }

    // ── Steps and progress ───────────────────────────────────────────────────
    void BuildStepRows()
    {
        for (int i = 0; i < stepRows.Length; i++)
        {
            var row = new Grid { Margin = new Thickness(0, 0, 0, 8), ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            var icon = new Panel { Width = 22, Height = 22, Margin = new Thickness(0, 0, 12, 0) };
            var ring = new Ellipse { StrokeThickness = 2 };
            var glyph = new Path
            {
                Stroke = Res("Bg"), StrokeThickness = 2.2, StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            };
            icon.Children.Add(ring);
            icon.Children.Add(glyph);
            var label = new TextBlock { VerticalAlignment = VerticalAlignment.Center, FontSize = 15 };
            Grid.SetColumn(label, 1);
            row.Children.Add(icon);
            row.Children.Add(label);
            StepsPanel.Children.Add(row);
            stepRows[i] = (ring, glyph, label);
            SetStep(i, StepState.Pending);
        }
    }

    static readonly Geometry Check = Geometry.Parse("M0,4 L3,7 L9,0");
    static readonly Geometry Cross = Geometry.Parse("M0,0 L7,7 M7,0 L0,7");

    void SetStep(int i, StepState s)
    {
        stepStates[i] = s;
        var (ring, glyph, label) = stepRows[i];
        ring.Classes.Set("running", s == StepState.Running);
        switch (s)
        {
            case StepState.Pending:
                ring.Stroke = Res("Line"); ring.Fill = Brushes.Transparent; glyph.Data = null;
                label.Foreground = Res("FgDim"); break;
            case StepState.Running:
                ring.Stroke = Res("Accent"); ring.Fill = Res("Accent"); glyph.Data = null;
                label.Foreground = Res("Fg"); break;
            case StepState.Done:
                ring.Stroke = Res("Good"); ring.Fill = Res("Good"); glyph.Data = Check;
                label.Foreground = Res("Fg"); break;
            case StepState.Failed:
                ring.Stroke = Res("Bad"); ring.Fill = Res("Bad"); glyph.Data = Cross;
                label.Foreground = Res("Bad"); break;
        }
    }

    void SetProgress(double? p)
    {
        Bar.IsIndeterminate = p == null && running;
        if (p != null) Bar.Value = p.Value * 100;
    }

    // ── Log ──────────────────────────────────────────────────────────────────
    void OpenLogFile()
    {
        lock (logFileLock)
        {
            if (logFile != null) return;
            try
            {
                Directory.CreateDirectory(EngineBase.WorkDir);
                logFile = new StreamWriter(LogPath, true, new UTF8Encoding(false)) { AutoFlush = true };
                logFile.WriteLine($"==== {DateTime.Now:yyyy-MM-dd HH:mm:ss}  SwitchWakerHD installer (Linux) ====");
            }
            catch { logFile = null; }
        }
    }

    void EnqueueLog(string msg, LogKind kind)
    {
        var prefix = kind switch { LogKind.Ok => "[OK] ", LogKind.Warn => "[!!] ", LogKind.Error => "[ERROR] ", LogKind.Info => ">> ", _ => "" };
        var line = prefix + msg;
        logQueue.Enqueue(line);
        lock (logFileLock) { try { logFile?.WriteLine(line); } catch { } }
    }

    void FlushLog()
    {
        if (logQueue.IsEmpty) return;
        while (logQueue.TryDequeue(out var line)) logText.Append(line).Append('\n');
        if (logText.Length > LogBoxMaxChars) logText.Remove(0, logText.Length - LogBoxMaxChars / 2);
        if (!logVisible) return;
        LogBox.Text = logText.ToString();
        LogBox.CaretIndex = LogBox.Text.Length;   // follows the end
    }

    bool logVisible;

    void ShowLog(bool show)
    {
        logVisible = show;
        LogBox.IsVisible = show;
        PageProgress.RowDefinitions[2].Height = show ? new GridLength(6, GridUnitType.Star) : new GridLength(0);
        PageProgress.RowDefinitions[0].Height = new GridLength(5, GridUnitType.Star);
        if (show) { LogBox.Text = logText.ToString(); FlushLog(); LogBox.CaretIndex = LogBox.Text?.Length ?? 0; }
        UpdateLogButtons();
    }

    void UpdateLogButtons()
    {
        BtnLog.Content = Str.Get(logVisible ? "btn_log_hide" : "btn_log_show");
        BtnLogCopy.Content = Str.Get("btn_log_copy");
        BtnLogCopy.IsVisible = logVisible;
        BtnLogOpen.Content = Str.Get("btn_log_open");
        BtnLogOpen.IsVisible = logVisible && File.Exists(LogPath) && FindOnPath("xdg-open") != null;
    }

    void BtnLog_Click(object? sender, RoutedEventArgs e) => ShowLog(!logVisible);

    async void BtnLogCopy_Click(object? sender, RoutedEventArgs e)
    {
        FlushLog();
        if (Clipboard != null) await Clipboard.SetTextAsync(logText.ToString());
    }

    void BtnLogOpen_Click(object? sender, RoutedEventArgs e) => XdgOpen(LogPath);

    // ── Done page: moving the files to the Switch ────────────────────────────
    // Three ways: DBI's MTP responder over USB (gio), the microSD in the PC (a card reader, or hekate's UMS),
    // or by hand. The device lists refresh themselves while the page is open.
    readonly DispatcherTimer devicePoll = new() { Interval = TimeSpan.FromSeconds(3) };
    bool xferBusy, mtpScanning, pollHooked;
    string[] mtpKeys = Array.Empty<string>(), sdKeys = Array.Empty<string>();

    void EnterDone()
    {
        TxtOut.Text = engine.SdOut;
        try
        {
            var files = Directory.GetFiles(engine.SdOut, "*", SearchOption.AllDirectories);
            TxtXferSize.Text = Str.Get("xfer_size", files.Length, (files.Sum(f => new FileInfo(f).Length) / 1e9).ToString("0.0"));
        }
        catch { TxtXferSize.Text = ""; }
        if (XferMtp.IsChecked != true && XferSd.IsChecked != true && XferManual.IsChecked != true)
            (GioMtp.Available ? XferMtp : XferSd).IsChecked = true;
        ShowPage(Page.Done);
        UpdatePanels();
        RefreshDevices();
        if (!pollHooked)
        {
            pollHooked = true;
            devicePoll.Tick += (_, _) => { if (page == Page.Done && !xferBusy) RefreshDevices(); };
        }
        devicePoll.Start();
    }

    void Xfer_Changed(object? sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { IsChecked: true }) return;
        UpdatePanels();
        if (page == Page.Done) RefreshDevices();
    }

    void UpdatePanels()
    {
        PanelMtp.IsVisible    = XferMtp.IsChecked == true;
        PanelSd.IsVisible     = XferSd.IsChecked == true;
        PanelManual.IsVisible = XferManual.IsChecked == true;
        TxtXferStatus.IsVisible = false;
        TxtNext1.Text = Str.Get(XferMtp.IsChecked == true ? "next_1_mtp" : "next_1");
        var panel = PanelMtp.IsVisible ? PanelMtp : PanelSd.IsVisible ? PanelSd : (Control)PanelManual;
        Dispatcher.UIThread.Post(() => panel.BringIntoView(), DispatcherPriority.Background);
    }

    void BtnDevRefresh_Click(object? sender, RoutedEventArgs e) => RefreshDevices();

    void RefreshDevices()
    {
        if (XferSd.IsChecked == true) RefreshDrives();
        if (XferMtp.IsChecked == true) RefreshMtp();
    }

    // Rebuilds a list of device cards only when what is connected changed, so the selection stays.
    void FillList(StackPanel list, ref string[] keys, (string key, string title, string info, object tag)[] items)
    {
        var newKeys = items.Select(i => i.key).ToArray();
        if (newKeys.SequenceEqual(keys)) return;
        var selected = list.Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true)?.Name;
        keys = newKeys;
        list.Children.Clear();
        foreach (var it in items)
        {
            var rb = new RadioButton
            {
                GroupName = list.Name, Tag = it.tag, Name = it.key, IsChecked = it.key == selected,
                Content = new StackPanel
                {
                    Children =
                    {
                        new TextBlock { Text = it.title, FontWeight = FontWeight.SemiBold },
                        new TextBlock { Text = it.info, Foreground = Res("FgDim"), FontSize = 13 },
                    },
                },
            };
            rb.Classes.Add("card");
            list.Children.Add(rb);
        }
        if (list.Children.Count > 0 && !list.Children.OfType<RadioButton>().Any(r => r.IsChecked == true))
            ((RadioButton)list.Children[0]).IsChecked = true;
    }

    sealed record Removable(string Mount, string Label, long Size, string FsType);

    // Mounted removable or hot-plugged block devices (card readers, hekate's UMS), from lsblk.
    static List<Removable> RemovableMounts()
    {
        var found = new List<Removable>();
        try
        {
            var psi = new ProcessStartInfo("lsblk", "-J -b -o PATH,MOUNTPOINT,RM,HOTPLUG,LABEL,SIZE,FSTYPE")
                { RedirectStandardOutput = true, UseShellExecute = false };
            using var p = Process.Start(psi)!;
            var json = p.StandardOutput.ReadToEnd();
            p.WaitForExit(5000);
            using var doc = JsonDocument.Parse(json);
            void Walk(JsonElement devs)
            {
                foreach (var d in devs.EnumerateArray())
                {
                    string? Str_(string k) => d.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
                    bool Flag(string k) => d.TryGetProperty(k, out var v) &&
                        (v.ValueKind == JsonValueKind.True || (v.ValueKind == JsonValueKind.String && v.GetString() == "1"));
                    long Num(string k) => d.TryGetProperty(k, out var v)
                        ? (v.ValueKind == JsonValueKind.Number ? v.GetInt64() : long.TryParse(v.GetString(), out var n) ? n : 0) : 0;
                    var mount = Str_("mountpoint");
                    if (!string.IsNullOrEmpty(mount) && mount != "/" && !mount.StartsWith("/boot") && mount != "[SWAP]" &&
                        (Flag("rm") || Flag("hotplug")))
                        found.Add(new Removable(mount!, Str_("label") ?? "", Num("size"), Str_("fstype") ?? ""));
                    if (d.TryGetProperty("children", out var ch)) Walk(ch);
                }
            }
            if (doc.RootElement.TryGetProperty("blockdevices", out var root)) Walk(root);
        }
        catch { }
        return found;
    }

    void RefreshDrives()
    {
        var drives = RemovableMounts();
        FillList(SdList, ref sdKeys, drives.Select(d => (
            "sd" + Math.Abs((d.Mount + d.Size).GetHashCode()),
            string.IsNullOrEmpty(d.Label) ? d.Mount : $"{d.Label}  ({d.Mount})",
            $"{d.Size / 1e9:0.#} GB · {d.FsType}",
            (object)d.Mount)).ToArray());
        TxtSdNone.IsVisible = drives.Count == 0;
        BtnCopySd.IsEnabled = drives.Count > 0 && !xferBusy;
    }

    async void RefreshMtp()
    {
        if (mtpScanning) return;
        mtpScanning = true;
        List<MtpStorage> found;
        try { found = await Task.Run(GioMtp.FindSdStorages); }
        catch { found = new List<MtpStorage>(); }
        finally { mtpScanning = false; }
        FillList(MtpList, ref mtpKeys, found.Select(m => ("mtp" + Math.Abs(m.Uri.GetHashCode()), m.Device, m.Name, (object)m)).ToArray());
        TxtMtpNone.IsVisible = found.Count == 0;
        BtnCopyMtp.IsEnabled = found.Count > 0 && !xferBusy;
    }

    void SetXferBusy(bool busy)
    {
        xferBusy = busy;
        running = busy;   // closing the window asks first
        foreach (var c in new Control[] { XferMtp, XferSd, XferManual, MtpList, SdList, BtnMtpRefresh, BtnSdRefresh })
            c.IsEnabled = !busy;
        BtnCopyMtp.IsEnabled = !busy && MtpList.Children.Count > 0;
        BtnCopySd.IsEnabled = !busy && SdList.Children.Count > 0;
        XferBar.IsVisible = busy;
        UpdateFooter();
    }

    void XferStatus(string text, string brush)
    {
        TxtXferStatus.Text = text;
        TxtXferStatus.Foreground = Res(brush);
        TxtXferStatus.IsVisible = true;
    }

    async void BtnCopySd_Click(object? sender, RoutedEventArgs e)
    {
        var mount = SdList.Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true)?.Tag as string;
        if (mount == null) return;
        SetXferBusy(true);
        XferBar.IsIndeterminate = false;
        XferBar.Value = 0;
        XferStatus(Str.Get("sd_copying", mount), "Fg");
        cts = new CancellationTokenSource();
        var token = cts.Token;
        try
        {
            await Task.Run(() => engine.CopyToSd(mount, (d, t) => Ui(() =>
            {
                XferBar.Value = t > 0 ? d * 100.0 / t : 0;
                TxtXferStatus.Text = Str.Get("sd_copying", mount) + $"  {d / 1048576} / {t / 1048576} MB";
            }), token));
            XferStatus(Str.Get("sd_copied_lx", mount), "Good");
        }
        catch (Exception ex)
        {
            XferStatus(ex is OperationCanceledException ? Str.Get("cancelled") : ex.Message, "Bad");
            EnqueueLog(ex.Message, LogKind.Error);
        }
        finally { SetXferBusy(false); }
    }

    async void BtnCopyMtp_Click(object? sender, RoutedEventArgs e)
    {
        var target = MtpList.Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true)?.Tag as MtpStorage;
        if (target == null) return;
        SetXferBusy(true);
        XferBar.IsIndeterminate = false;
        XferBar.Value = 0;
        XferStatus(Str.Get("mtp_starting", target.Device), "Fg");
        EnqueueLog(Str.Get("mtp_starting", target.Device) + $" ({target.Uri})", LogKind.Info);
        cts = new CancellationTokenSource();
        var token = cts.Token;
        var src = engine.SdOut;
        try
        {
            await Task.Run(() => GioMtp.Copy(src, target, (fd, ft, bd, bt, name) => Ui(() =>
            {
                XferBar.Value = bt > 0 ? bd * 100.0 / bt : 0;
                TxtXferStatus.Text = Str.Get("mtp_progress", fd, ft, bd / 1048576, bt / 1048576);
            }), line => EnqueueLog(line, LogKind.Output), token));
            XferStatus(Str.Get("mtp_done"), "Good");
            EnqueueLog(Str.Get("mtp_done"), LogKind.Ok);
        }
        catch (Exception ex)
        {
            var msg = ex is OperationCanceledException ? Str.Get("cancelled")
                    : ex is InstallException ? ex.Message
                    : Str.Get("mtp_error", ex.Message);
            XferStatus(msg, "Bad");
            EnqueueLog(msg, LogKind.Error);
        }
        finally { SetXferBusy(false); }
    }

    void BtnOpenOut_Click(object? sender, RoutedEventArgs e) => XdgOpen(engine.SdOut);

    static void XdgOpen(string path)
    {
        try { Process.Start(new ProcessStartInfo("xdg-open") { ArgumentList = { path }, UseShellExecute = false }); } catch { }
    }

    static string? FindOnPath(string name) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':')
            .Where(d => d.Length > 0).Select(d => System.IO.Path.Combine(d, name)).FirstOrDefault(File.Exists);
}
