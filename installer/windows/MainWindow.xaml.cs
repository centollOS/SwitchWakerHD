using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;

namespace SwitchWakerHD.Installer;

public partial class MainWindow : Window
{
    enum Page { Welcome, Game, Progress, Done }

    readonly InstallEngine engine = new();
    Page page = Page.Welcome;
    bool running, failed;
    CancellationTokenSource? cts;
    ManualResetEventSlim? noticeEvent;

    // The log: lines queue up from the worker thread and reach the TextBox in batches; all of it also goes to a file.
    readonly ConcurrentQueue<string> logQueue = new();
    readonly DispatcherTimer logTimer = new() { Interval = TimeSpan.FromMilliseconds(200) };
    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch elapsed = new();
    readonly object logFileLock = new();
    StreamWriter? logFile;
    const int LogBoxMaxChars = 400_000;
    static readonly string LogPath = System.IO.Path.Combine(InstallEngine.WorkDir, "installer.log");

    readonly (Ellipse ring, TextBlock glyph, TextBlock label)[] stepRows = new (Ellipse, TextBlock, TextBlock)[4];
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

        SourceInitialized += (_, _) => UseDarkTitleBar();
        Closing += Window_Closing;
        Drop += Window_Drop;

        OptWua.IsChecked = true;
        ShowPage(Page.Welcome);
        ApplyLanguage();
    }

    void Ui(Action a) => Dispatcher.BeginInvoke(a);

    // ── Pages and texts ──────────────────────────────────────────────────────
    void ShowPage(Page p)
    {
        page = p;
        PageWelcome.Visibility  = p == Page.Welcome  ? Visibility.Visible : Visibility.Collapsed;
        PageGame.Visibility     = p == Page.Game     ? Visibility.Visible : Visibility.Collapsed;
        PageProgress.Visibility = p == Page.Progress ? Visibility.Visible : Visibility.Collapsed;
        PageDone.Visibility     = p == Page.Done     ? Visibility.Visible : Visibility.Collapsed;
        BtnLang.Visibility      = p == Page.Welcome || p == Page.Game ? Visibility.Visible : Visibility.Collapsed;
        UpdateFooter();
    }

    void UpdateFooter()
    {
        void Set(Button b, string? key, bool enabled = true)
        {
            b.Visibility = key == null ? Visibility.Collapsed : Visibility.Visible;
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
            case Page.Done:     Set(BtnSecondary, null);         Set(BtnPrimary, "btn_close"); break;
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
        TxtWelcomeLi1.Text   = "•  " + Str.Get("welcome_li1");
        TxtWelcomeLi2.Text   = "•  " + Str.Get("welcome_li2");
        TxtWelcomeLi3.Text   = "•  " + Str.Get("welcome_li3");
        TxtWelcomeNeed.Text  = Str.Get("welcome_need");
        TxtWelcomeDir.Text   = Str.Get("welcome_dir", InstallEngine.WorkDir);

        TxtGameH.Text    = Str.Get("game_h");
        TxtGameBody.Text = Str.Get("game_body");
        TxtOptWua.Text   = Str.Get("game_opt_wua");  TxtOptWuaD.Text = Str.Get("game_opt_wua_d");
        TxtOptImg.Text   = Str.Get("game_opt_img");  TxtOptImgD.Text = Str.Get("game_opt_img_d");
        TxtOptDir.Text   = Str.Get("game_opt_dir");  TxtOptDirD.Text = Str.Get("game_opt_dir_d");
        BtnBrowse.Content = Str.Get("btn_browse");
        ValidateGame();
        TxtFwdH.Text = Str.Get("fwd_h");
        TxtFwdD.Text = Str.Get("fwd_d");
        BtnFwdPick.Content = Str.Get("fwd_pick");
        BtnFwdClear.Content = Str.Get("fwd_clear");
        ShowKeys();
        TxtShH.Text = Str.Get("sh_h");
        TxtShD.Text = Str.Get("sh_d");
        TxtMmH.Text = Str.Get("mm_h");
        TxtMmD.Text = Str.Get("mm_d");
        BtnMmPick.Content = Str.Get("mm_pick");
        BtnMmFolder.Content = Str.Get("mm_folder");
        BtnMmClear.Content = Str.Get("fwd_clear");
        ShowMinimap();

        TxtProgH.Text    = Str.Get("prog_h");
        TxtProgBody.Text = Str.Get("prog_body");
        for (int i = 0; i < stepRows.Length; i++) stepRows[i].label.Text = Str.Get(InstallEngine.StepKeys[i]);
        TxtErrorH.Text   = Str.Get("fail_h");
        TxtLogPath.Text  = Str.Get("log_at", LogPath);
        UpdateLogButtons();

        TxtDoneH.Text     = Str.Get("done_h");
        TxtDoneBody.Text  = Str.Get("done_body");
        TxtXferSd.Text  = Str.Get("xfer_sd");        TxtXferSdD.Text  = Str.Get("xfer_sd_d");
        TxtXferManual.Text = Str.Get("xfer_manual"); TxtXferManualD.Text = Str.Get("xfer_manual_d");
        TxtSdHint.Text    = Str.Get("sd_hint");
        TxtSdLabel.Text   = Str.Get("sd_label");
        BtnSdRefresh.Content = Str.Get("btn_refresh");
        TxtSdNone.Text    = Str.Get("sd_none");
        BtnCopySd.Content = Str.Get("btn_copy_sd");
        TxtManual.Text    = Str.Get("manual_body");
        BtnOpenOut.Content = Str.Get("btn_open_out");
        TxtNextH.Text = Str.Get("next_h");
        TxtNext1.Text = Str.Get("next_1"); TxtNext2.Text = Str.Get("next_2");
        TxtNext3.Text = Str.Get("next_3"); TxtNext4.Text = Str.Get("next_4");

        UpdateFooter();
    }

    void BtnLang_Click(object sender, RoutedEventArgs e)
    {
        Str.Es = !Str.Es;
        ApplyLanguage();
    }

    void BtnPrimary_Click(object sender, RoutedEventArgs e)
    {
        switch (page)
        {
            case Page.Welcome:  ShowPage(Page.Game); break;
            case Page.Game:     StartInstall(); break;
            case Page.Progress: if (!running && failed) StartInstall(); break;
            case Page.Done:     Close(); break;
        }
    }

    void BtnSecondary_Click(object sender, RoutedEventArgs e)
    {
        switch (page)
        {
            case Page.Game:     ShowPage(Page.Welcome); break;
            case Page.Progress: if (running) AskCancel(); else Close(); break;
        }
    }

    // ── Game page ────────────────────────────────────────────────────────────
    string gamePath = "";

    void Opt_Checked(object sender, RoutedEventArgs e)
    {
        // A path picked for another format no longer fits.
        if (gamePath != "" && !PathFitsFormat(gamePath)) gamePath = "";
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
        if (TxtGamePath == null) return;   // during InitializeComponent
        TxtGamePath.Text = gamePath == "" ? Str.Get("game_none_yet") : gamePath;
        TxtGamePath.Foreground = (Brush)FindResource(gamePath == "" ? "FgDim" : "Fg");

        string? warn = null;
        if (gamePath != "" && GameArg() == "--game-dir" && !IsGameDir(gamePath)) warn = Str.Get("game_bad_dir");
        else if (gamePath != "" && GameArg() == "--image" && File.Exists(gamePath))
        {
            try { warn = InstallEngine.CheckImageKeys(gamePath); }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { warn = e.Message; }
        }
        TxtGameWarn.Text = warn ?? "";
        TxtGameWarn.Visibility = warn == null ? Visibility.Collapsed : Visibility.Visible;
        if (page == Page.Game) UpdateFooter();
    }

    void BtnBrowse_Click(object sender, RoutedEventArgs e)
    {
        if (GameArg() == "--game-dir")
        {
            using var dlg = new System.Windows.Forms.FolderBrowserDialog { Description = Str.Get("dlg_folder"), ShowNewFolderButton = false };
            if (gamePath != "" && Directory.Exists(gamePath)) dlg.SelectedPath = gamePath;
            if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) gamePath = dlg.SelectedPath;
        }
        else
        {
            var img = GameArg() == "--image";
            var dlg = new OpenFileDialog
            {
                Title  = Str.Get(img ? "dlg_title_img" : "dlg_title_wua"),
                Filter = Str.Get(img ? "dlg_filter_img" : "dlg_filter_wua"),
            };
            if (dlg.ShowDialog(this) == true) gamePath = dlg.FileName;
        }
        ValidateGame();
    }

    // ── Optional HOME-screen forwarder: the console's prod.keys ─────────────
    string? keysPath;

    void ShowKeys()
    {
        TxtFwdKeys.Text = keysPath ?? Str.Get("fwd_none");
        TxtFwdKeys.Foreground = (Brush)FindResource(keysPath == null ? "FgDim" : "Fg");
        BtnFwdClear.Visibility = keysPath == null ? Visibility.Collapsed : Visibility.Visible;
    }

    void BtnFwdPick_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Title = Str.Get("fwd_dlg_title"), Filter = Str.Get("fwd_dlg_filter") };
        if (dlg.ShowDialog(this) == true) keysPath = dlg.FileName;
        ShowKeys();
    }

    void BtnFwdClear_Click(object sender, RoutedEventArgs e)
    {
        keysPath = null;
        ShowKeys();
    }

    // ── Optional GameCube sea minimap: the player's GameCube Wind Waker ─────
    string? minimapGame;

    void ShowMinimap()
    {
        TxtMmGame.Text = minimapGame ?? Str.Get("mm_none");
        TxtMmGame.Foreground = (Brush)FindResource(minimapGame == null ? "FgDim" : "Fg");
        BtnMmClear.Visibility = minimapGame == null ? Visibility.Collapsed : Visibility.Visible;
        // a Dolphin .rvz (Zstandard) also gets Python 3.14, which the map tool needs for it
        var rvz = minimapGame != null && InstallEngine.IsZstdRvz(minimapGame);
        TxtMmNote.Text = rvz ? Str.Get("mm_rvz_note") : "";
        TxtMmNote.Visibility = rvz ? Visibility.Visible : Visibility.Collapsed;
    }

    void BtnMmPick_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Title = Str.Get("mm_dlg_title"), Filter = Str.Get("mm_dlg_filter") };
        if (dlg.ShowDialog(this) == true) minimapGame = dlg.FileName;
        ShowMinimap();
    }

    void BtnMmFolder_Click(object sender, RoutedEventArgs e)
    {
        using var dlg = new System.Windows.Forms.FolderBrowserDialog { Description = Str.Get("mm_dlg_folder"), ShowNewFolderButton = false };
        if (minimapGame != null && Directory.Exists(minimapGame)) dlg.SelectedPath = minimapGame;
        if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) minimapGame = dlg.SelectedPath;
        ShowMinimap();
    }

    void BtnMmClear_Click(object sender, RoutedEventArgs e)
    {
        minimapGame = null;
        ShowMinimap();
    }

    // Dropping the game file or folder on the window picks it and its format.
    void Window_Drop(object sender, DragEventArgs e)
    {
        if (running || page == Page.Done || !e.Data.GetDataPresent(DataFormats.FileDrop)) return;
        var path = ((string[])e.Data.GetData(DataFormats.FileDrop)).FirstOrDefault();
        if (path == null) return;
        var ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        if (Directory.Exists(path)) OptDir.IsChecked = true;
        else if (ext == ".wua") OptWua.IsChecked = true;
        else if (ext == ".wux" || ext == ".wud") OptImg.IsChecked = true;
        else return;
        gamePath = path;
        if (page != Page.Game) ShowPage(Page.Game);
        ValidateGame();
    }

    // ── Install ──────────────────────────────────────────────────────────────
    async void StartInstall()
    {
        engine.GamePath = gamePath;
        engine.GameArg  = GameArg();
        engine.ForwarderKeys = keysPath != null && File.Exists(keysPath) ? keysPath : null;
        engine.MinimapGame = minimapGame;
        engine.PrepareShaders = ChkShaders.IsChecked == true;
        running = true; failed = false;
        cts = new CancellationTokenSource();
        for (int i = 0; i < stepStates.Length; i++) SetStep(i, StepState.Pending);
        ErrorBox.Visibility = Visibility.Collapsed;
        Notice.Visibility = Visibility.Collapsed;
        TxtStatus.Foreground = (Brush)FindResource("Fg");
        OpenLogFile();
        ShowPage(Page.Progress);
        elapsed.Restart();
        clock.Start();

        string? error = null;
        try
        {
            await Task.Run(() => engine.Run(cts.Token));
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
            FlashIfInactive();
            return;
        }

        failed = true;
        EnqueueLog(error, LogKind.Error);
        TxtError.Text = error;
        ErrorBox.Visibility = Visibility.Visible;
        TxtStatus.Text = "";
        SetProgress(0);
        for (int i = 0; i < stepStates.Length; i++)
            if (stepStates[i] == StepState.Running) SetStep(i, StepState.Failed);
        ShowLog(true);
        UpdateFooter();
        FlashIfInactive();
    }

    void AskCancel()
    {
        if (MessageBox.Show(this, Str.Get("cancel_q"), Str.Get("title"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;
        BtnSecondary.IsEnabled = false;
        TxtStatus.Text = Str.Get("cancelling");
        cts?.Cancel();
        engine.KillCurrent();
    }

    void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (running)
        {
            if (MessageBox.Show(this, Str.Get("cancel_q"), Str.Get("title"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            cts?.Cancel();
            engine.KillCurrent();
        }
        FlushLog();
        lock (logFileLock) { logFile?.Dispose(); logFile = null; }
    }

    // Called on the worker thread: shows the notice and waits for its button (or a cancel).
    void AskUser(string heading, string text, string button)
    {
        var ev = new ManualResetEventSlim();
        noticeEvent = ev;
        Dispatcher.Invoke(() =>
        {
            TxtNoticeH.Text = heading;
            TxtNotice.Text = text;
            BtnNotice.Content = button;
            Notice.Visibility = Visibility.Visible;
            Notice.BringIntoView();
            FlashIfInactive();
        });
        try { ev.Wait(cts!.Token); }
        finally
        {
            noticeEvent = null;
            Dispatcher.Invoke(() => Notice.Visibility = Visibility.Collapsed);
        }
    }

    void BtnNotice_Click(object sender, RoutedEventArgs e) => noticeEvent?.Set();

    // ── Steps and progress ───────────────────────────────────────────────────
    void BuildStepRows()
    {
        for (int i = 0; i < stepRows.Length; i++)
        {
            var row = new Grid { Margin = new Thickness(0, 0, 0, 8) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            var icon = new Grid { Width = 22, Height = 22, Margin = new Thickness(0, 0, 12, 0) };
            var ring = new Ellipse { StrokeThickness = 2 };
            var glyph = new TextBlock
            {
                FontFamily = new FontFamily("Segoe MDL2 Assets"), FontSize = 11, Foreground = (Brush)FindResource("Bg"),
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

    void SetStep(int i, StepState s)
    {
        stepStates[i] = s;
        var (ring, glyph, label) = stepRows[i];
        ring.BeginAnimation(OpacityProperty, null);
        ring.Opacity = 1;
        switch (s)
        {
            case StepState.Pending:
                ring.Stroke = (Brush)FindResource("Line"); ring.Fill = Brushes.Transparent; glyph.Text = "";
                label.Foreground = (Brush)FindResource("FgDim"); break;
            case StepState.Running:
                ring.Stroke = (Brush)FindResource("Accent"); ring.Fill = (Brush)FindResource("Accent"); glyph.Text = "";
                label.Foreground = (Brush)FindResource("Fg");
                ring.BeginAnimation(OpacityProperty, new DoubleAnimation(1, 0.3, TimeSpan.FromSeconds(0.8))
                    { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
                break;
            case StepState.Done:
                ring.Stroke = (Brush)FindResource("Good"); ring.Fill = (Brush)FindResource("Good"); glyph.Text = "";
                label.Foreground = (Brush)FindResource("Fg"); break;
            case StepState.Failed:
                ring.Stroke = (Brush)FindResource("Bad"); ring.Fill = (Brush)FindResource("Bad"); glyph.Text = "";
                label.Foreground = (Brush)FindResource("Bad"); break;
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
                Directory.CreateDirectory(InstallEngine.WorkDir);
                // Shared: a second installer window (or Notepad on an older version's lock) appends to the same file.
                var stream = new FileStream(LogPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                logFile = new StreamWriter(stream, Encoding.UTF8) { AutoFlush = true };
                logFile.WriteLine($"==== {DateTime.Now:yyyy-MM-dd HH:mm:ss}  SwitchWakerHD installer (GUI) ====");
            }
            catch (Exception e)
            {
                logFile = null;
                EnqueueLog(Str.Get("log_open_fail", LogPath, e.Message), LogKind.Warn);
            }
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
        var sb = new StringBuilder();
        while (logQueue.TryDequeue(out var line)) sb.Append(line).Append('\n');
        var atEnd = LogBox.VerticalOffset + LogBox.ViewportHeight >= LogBox.ExtentHeight - 4;
        LogBox.AppendText(sb.ToString());
        if (LogBox.Text.Length > LogBoxMaxChars)
            LogBox.Text = LogBox.Text.Substring(LogBox.Text.Length - LogBoxMaxChars / 2);
        if (atEnd) LogBox.ScrollToEnd();
    }

    bool logVisible;

    void ShowLog(bool show)
    {
        logVisible = show;
        LogBox.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        LogRow.Height = show ? new GridLength(6, GridUnitType.Star) : new GridLength(0);
        if (show) { FlushLog(); _ = Dispatcher.BeginInvoke(new Action(LogBox.ScrollToEnd), DispatcherPriority.Loaded); }
        UpdateLogButtons();
    }

    void UpdateLogButtons()
    {
        BtnLog.Content = Str.Get(logVisible ? "btn_log_hide" : "btn_log_show");
        BtnLogCopy.Content = Str.Get("btn_log_copy");
        BtnLogCopy.Visibility = logVisible ? Visibility.Visible : Visibility.Collapsed;
        BtnLogOpen.Content = Str.Get("btn_log_open");
        BtnLogOpen.Visibility = logVisible && File.Exists(LogPath) ? Visibility.Visible : Visibility.Collapsed;
    }

    void BtnLog_Click(object sender, RoutedEventArgs e) => ShowLog(!logVisible);

    void BtnLogCopy_Click(object sender, RoutedEventArgs e)
    {
        FlushLog();
        try { Clipboard.SetText(LogBox.Text); } catch { }
    }

    void BtnLogOpen_Click(object sender, RoutedEventArgs e)
    {
        try { Process.Start("notepad.exe", $"\"{LogPath}\""); } catch { }
    }

    // ── Done page: moving the files to the Switch ────────────────────────────
    // Two ways: the microSD in the PC (a card reader, or hekate's UMS), or by hand. The card list refreshes itself
    // while the page is open. (No copy over DBI's MTP: it failed silently and could not be read back.)
    readonly DispatcherTimer devicePoll = new() { Interval = TimeSpan.FromSeconds(3) };
    bool xferBusy;
    string[] sdKeys = new string[0];

    void EnterDone()
    {
        TxtOut.Text = engine.SdOut;
        // The forwarder lands in the SD folder's NSP\ and travels with it, whichever way is chosen.
        var nsp = System.IO.Path.Combine(engine.SdOut, "NSP", "wwhd_forwarder.nsp");
        var nspFresh = File.Exists(nsp) && File.GetLastWriteTime(nsp) > DateTime.Now - elapsed.Elapsed - TimeSpan.FromMinutes(1);
        TxtFwdDone.Visibility = keysPath != null ? Visibility.Visible : Visibility.Collapsed;
        TxtFwdDone.Text = Str.Get(nspFresh ? "fwd_done" : "fwd_missing");
        TxtFwdDone.Foreground = (Brush)FindResource(nspFresh ? "Fg" : "Warn");
        TxtMmDone.Visibility = minimapGame != null ? Visibility.Visible : Visibility.Collapsed;
        TxtMmDone.Text = Str.Get("mm_done");
        TxtGameFilesDone.Visibility = engine.BadGameFiles > 0 ? Visibility.Visible : Visibility.Collapsed;
        TxtGameFilesDone.Text = Str.Get("game_files_done", engine.BadGameFiles, string.Join("\n", engine.BadGameFileLines.Take(3)));
        TxtGameFilesDone.Foreground = (Brush)FindResource("Warn");
        try
        {
            var files = Directory.GetFiles(engine.SdOut, "*", SearchOption.AllDirectories);
            TxtXferSize.Text = Str.Get("xfer_size", files.Length, (files.Sum(f => new FileInfo(f).Length) / 1e9).ToString("0.0"));
        }
        catch { TxtXferSize.Text = ""; }
        if (XferSd.IsChecked != true && XferManual.IsChecked != true) XferSd.IsChecked = true;
        ShowPage(Page.Done);
        RefreshDevices();
        if (devicePoll.Tag == null)
        {
            devicePoll.Tag = true;
            devicePoll.Tick += (_, _) => { if (page == Page.Done && !xferBusy) RefreshDevices(); };
        }
        devicePoll.Start();
    }

    void Xfer_Checked(object sender, RoutedEventArgs e)
    {
        PanelSd.Visibility     = XferSd.IsChecked == true     ? Visibility.Visible : Visibility.Collapsed;
        PanelManual.Visibility = XferManual.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        TxtXferStatus.Visibility = Visibility.Collapsed;
        if (page == Page.Done)
        {
            RefreshDevices();
            var panel = XferSd.IsChecked == true ? PanelSd : PanelManual;
            _ = Dispatcher.BeginInvoke(new Action(() => panel.BringIntoView()), DispatcherPriority.Loaded);
        }
    }

    void BtnDevRefresh_Click(object sender, RoutedEventArgs e) => RefreshDevices();

    void RefreshDevices()
    {
        if (XferSd.IsChecked == true) RefreshDrives();
    }

    // Rebuilds a list of device cards only when what is connected changed, so the selection stays.
    void FillList(StackPanel list, ref string[] keys, (string key, string title, string info, object tag)[] items)
    {
        var newKeys = items.Select(i => i.key).ToArray();
        if (newKeys.SequenceEqual(keys)) return;
        var selected = list.Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true)?.Uid;
        keys = newKeys;
        list.Children.Clear();
        foreach (var it in items)
        {
            list.Children.Add(new RadioButton
            {
                Style = (Style)FindResource("ChoiceCard"), GroupName = list.Name, Tag = it.tag, Uid = it.key,
                IsChecked = it.key == selected,
                Content = new StackPanel
                {
                    Children =
                    {
                        new TextBlock { Text = it.title, FontWeight = FontWeights.SemiBold },
                        new TextBlock { Text = it.info, Foreground = (Brush)FindResource("FgDim"), FontSize = 13 },
                    },
                },
            });
        }
        if (list.Children.Count > 0 && !list.Children.OfType<RadioButton>().Any(r => r.IsChecked == true))
            ((RadioButton)list.Children[0]).IsChecked = true;
    }

    void RefreshDrives()
    {
        var drives = DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.Removable && d.IsReady).ToList();
        FillList(SdPanel, ref sdKeys, drives.Select(d => (
            d.Name + d.VolumeLabel + d.TotalSize,
            string.IsNullOrEmpty(d.VolumeLabel) ? d.Name : $"{d.Name}  {d.VolumeLabel}",
            $"{d.TotalSize / 1e9:0.#} GB · {d.DriveFormat}",
            (object)d.Name)).ToArray());
        TxtSdNone.Visibility = drives.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        BtnCopySd.IsEnabled = drives.Count > 0 && !xferBusy;
    }

    void SetXferBusy(bool busy)
    {
        xferBusy = busy;
        running = busy;   // closing the window asks first
        foreach (var c in new UIElement[] { XferSd, XferManual, SdPanel, BtnSdRefresh, BtnPrimary })
            c.IsEnabled = !busy;
        BtnCopySd.IsEnabled = !busy && SdPanel.Children.Count > 0;
        XferBar.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
    }

    void XferStatus(string text, string brush)
    {
        TxtXferStatus.Text = text;
        TxtXferStatus.Foreground = (Brush)FindResource(brush);
        TxtXferStatus.Visibility = Visibility.Visible;
    }

    async void BtnCopySd_Click(object sender, RoutedEventArgs e)
    {
        var drive = SdPanel.Children.OfType<RadioButton>().FirstOrDefault(r => r.IsChecked == true)?.Tag as string;
        if (drive == null) return;
        SetXferBusy(true);
        XferBar.IsIndeterminate = true;
        XferStatus(Str.Get("sd_copying", drive), "Fg");
        cts = new CancellationTokenSource();
        try
        {
            await Task.Run(() => engine.CopyToSd(drive, (d, t) => Ui(() =>
            {
                XferBar.IsIndeterminate = false;
                XferBar.Value = t > 0 ? d * 100.0 / t : 100;
                TxtXferStatus.Text = Str.Get("sd_verify_prog") + $"  {d / 1048576} / {t / 1048576} MB";
            }), cts.Token));
            XferStatus(Str.Get("sd_copied", drive), "Good");
        }
        catch (Exception ex)
        {
            var msg = ex is OperationCanceledException ? Str.Get("cancelled") : ex.Message;
            XferStatus(msg, "Bad");
            EnqueueLog(msg, LogKind.Error);
            if (!(ex is InstallException || ex is OperationCanceledException)) EnqueueLog(ex.ToString(), LogKind.Error);
        }
        finally { SetXferBusy(false); }
    }

    void BtnOpenOut_Click(object sender, RoutedEventArgs e)
    {
        try { Process.Start("explorer.exe", $"\"{engine.SdOut}\""); } catch { }
    }

    // ── Window chrome ────────────────────────────────────────────────────────
    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [DllImport("user32.dll")]
    static extern bool FlashWindow(IntPtr hwnd, bool invert);

    void UseDarkTitleBar()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        int on = 1;
        // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+); 19 on earlier Windows 10 builds.
        if (DwmSetWindowAttribute(hwnd, 20, ref on, sizeof(int)) != 0)
            DwmSetWindowAttribute(hwnd, 19, ref on, sizeof(int));
    }

    void FlashIfInactive()
    {
        if (!IsActive) FlashWindow(new WindowInteropHelper(this).Handle, true);
    }
}
