// The manager's window: sidebar, header art, and the pages (Home, Mods, Saves, Log,
// Settings, Build a mod, About), laid out like manager/mockup.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace DmfdManager
{
    partial class MainWindow : Window
    {
        public const string Version = "0.10";
        readonly Grid root = new Grid();
        readonly StackPanel nav = new StackPanel();
        readonly ScrollViewer scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Grid mainArea, heroHolder;
        Image heroImg;
        readonly Border page = new Border { Padding = new Thickness(52, 46, 52, 60), MaxWidth = 1320, HorizontalAlignment = HorizontalAlignment.Left };
        readonly Border busy = new Border { Visibility = Visibility.Collapsed };
        readonly TextBlock busyTitle = Ui.H("Preparing mods...", 18), busyLine = Ui.T("", 12.5, Theme.Muted);
        bool combineAgain;            // more changes came in while preparing: prepare again after
        readonly ProgressBar busyBar = new ProgressBar { Minimum = 0, Maximum = 100 };
        DispatcherTimer busyTick;
        Action afterCombine;          // e.g. starting the game once the mods are ready
        readonly Border statusDot = new Border { Width = 8, Height = 8, CornerRadius = new CornerRadius(4), Margin = new Thickness(0, 0, 8, 0) };
        readonly TextBlock statusText = Ui.T("", 13, Theme.Muted);
        string current = "home";
        List<string> lastOverlaps = new List<string>();
        string lastCombineProblem;
        DispatcherTimer combineSoon;
        bool working;

        public MainWindow()
        {
            Title = "DMFD Mod Manager";
            Width = 1440;
            Height = 920;
            MinWidth = 1280;  // the mod list's columns all fit
            MinHeight = 680;
            Background = Theme.Bg;
            FontFamily = Theme.Sans;
            Content = root;
            AllowDrop = true;
            Drop += OnDrop;
            DragOver += (s, e) => { e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; e.Handled = true; };
            Build();
            Show("home");
            SourceInitialized += (s, e) => Dialogs.DarkTitle(this);
            // the game closing: prepare the mods if they changed meanwhile, and redraw (cards about the game)
            bool wasRunning = Launcher.GameRunning();
            var gameWatch = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            gameWatch.Tick += (s, e) =>
            {
                bool running = Launcher.GameRunning();
                if (running == wasRunning) return;
                wasRunning = running;
                Log.Write(running ? "the game started" : "the game closed");
                if (!running && Paths.GameOk && !working && !Combiner.UpToDate(ModStore.List())) Combine(null);
                else Refresh();
            };
            gameWatch.Start();
            if (Paths.GameOk && ModList.Current == null) LoadListSoon();  // updates on Home, mods a room needs
            Icon = AppIcon();
        }

        // ---- frame ----
        void Build()
        {
            root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(260) });
            root.ColumnDefinitions.Add(new ColumnDefinition());

            // sidebar
            var side = new DockPanel { Background = Theme.Side, LastChildFill = false };
            var brand = new StackPanel { Margin = new Thickness(14, 18, 14, 22) };
            string logo = Paths.LogoArt;
            if (logo != null)
                brand.Children.Add(new Image { Source = Load(logo), Height = 132, Stretch = Stretch.Uniform });
            else
            {
                var name = Ui.T("Dragon Marked\nfor Death", 26, Theme.Text, true, Theme.Serif);
                name.TextAlignment = TextAlignment.Center;
                brand.Children.Add(name);
            }
            var sub = Ui.T("MOD MANAGER", 11.5, Theme.Gold, true);
            sub.TextAlignment = TextAlignment.Center;
            sub.Margin = new Thickness(0, 4, 0, 0);
            brand.Children.Add(sub);
            DockPanel.SetDock(brand, Dock.Top);
            side.Children.Add(brand);
            nav.Margin = new Thickness(14, 0, 14, 0);
            DockPanel.SetDock(nav, Dock.Top);
            side.Children.Add(nav);

            var bottom = new StackPanel { Margin = new Thickness(14, 0, 14, 16) };
            var launch = Ui.Btn("▶  Launch game", () => Launch(false), true);
            ((TextBlock)launch.Child).FontSize = 15;
            ((TextBlock)launch.Child).HorizontalAlignment = HorizontalAlignment.Center;
            launch.Padding = new Thickness(13);
            bottom.Children.Add(launch);
            var vanilla = Ui.Btn("Launch without mods", () => Launch(true), false, true, "Starts the game once with every mod switched off. Your mod list is kept.");
            ((TextBlock)vanilla.Child).HorizontalAlignment = HorizontalAlignment.Center;
            vanilla.Margin = new Thickness(0, 8, 0, 0);
            bottom.Children.Add(vanilla);
            var status = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(8, 16, 0, 0) };
            status.Children.Add(statusDot);
            status.Children.Add(statusText);
            bottom.Children.Add(new Border { BorderBrush = Theme.LineSoft, BorderThickness = new Thickness(0, 1, 0, 0), Margin = new Thickness(0, 16, 0, 0), Child = status });
            var ver = Ui.T("Mod Manager " + Version, 12, Theme.Faint);
            ver.Margin = new Thickness(24, 2, 0, 0);
            bottom.Children.Add(ver);
            DockPanel.SetDock(bottom, Dock.Bottom);
            side.Children.Add(bottom);
            root.Children.Add(side);

            // main area: header art behind the page
            var main = new Grid { ClipToBounds = true };
            mainArea = main;
            main.SizeChanged += (s, e) => FitIntro();
            Grid.SetColumn(main, 1);
            string hero = Paths.HeroArt;
            if (hero != null)
            {
                var img = new Image { Source = Load(hero), Height = 360, Stretch = Stretch.UniformToFill, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Opacity = 0.9 };
                // fades out at its left and bottom edges (no hard line on a wide window): background-
                // coloured fades laid over the picture, sized by what's shown (an opacity mask is
                // sized by the whole picture, which can be wider than what's shown)
                var holder = new Grid { Height = 360, VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Right, MaxWidth = 1115, ClipToBounds = true };
                heroImg = img;
                heroHolder = holder;
                holder.Children.Add(img);
                var leftFade = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
                leftFade.GradientStops.Add(new GradientStop(Theme.C("#120b0d"), 0));
                leftFade.GradientStops.Add(new GradientStop(Theme.C("#00120b0d"), 0.55));
                holder.Children.Add(new Border { Background = leftFade });
                var bottomFade = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
                // fade from the background colour made transparent: WPF's Transparent is transparent
                // WHITE, which made a light band halfway down
                bottomFade.GradientStops.Add(new GradientStop(Theme.C("#00120b0d"), 0.45));
                bottomFade.GradientStops.Add(new GradientStop(Theme.C("#120b0d"), 1));
                holder.Children.Add(new Border { Background = bottomFade });
                main.Children.Add(holder);
            }
            else
            {
                // no Steam art: a red-to-blue glow like the title screen
                var glow = new RadialGradientBrush { Center = new Point(0.85, 0.1), GradientOrigin = new Point(0.85, 0.1), RadiusX = 0.6, RadiusY = 0.9 };
                glow.GradientStops.Add(new GradientStop(Theme.C("#55d42a35"), 0));
                glow.GradientStops.Add(new GradientStop(Theme.C("#226a2bb8"), 0.5));
                glow.GradientStops.Add(new GradientStop(Theme.C("#006a2bb8"), 1));
                main.Children.Add(new Border { Height = 360, VerticalAlignment = VerticalAlignment.Top, Background = glow });
            }
            scroll.Content = page;
            main.Children.Add(scroll);

            // "Preparing mods..." in the corner: the manager stays usable meanwhile
            busyBar.Height = 6;
            busyBar.Foreground = Theme.Red;
            busyBar.Background = Theme.B("#2a1c1f");
            busyBar.BorderThickness = new Thickness(0);
            busyBar.Margin = new Thickness(0, 12, 0, 10);
            var bar = busyBar;
            busy.Child = Ui.Stack(busyTitle, bar, busyLine);
            busy.Background = Theme.PanelSolid;
            busy.BorderBrush = Theme.Gold;
            busy.BorderThickness = new Thickness(1);
            busy.CornerRadius = new CornerRadius(8);
            busy.Padding = new Thickness(20, 14, 20, 14);
            busy.Width = 420;
            busy.HorizontalAlignment = HorizontalAlignment.Right;
            busy.VerticalAlignment = VerticalAlignment.Bottom;
            busy.Margin = new Thickness(0, 0, 26, 22);
            busy.Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 24, ShadowDepth = 4, Opacity = 0.5, Color = Colors.Black };
            main.Children.Add(busy);

            // notices
            Notice.Host = new Border
            {
                Background = Theme.PanelSolid, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6), Padding = new Thickness(18, 12, 18, 12),
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, 22), Visibility = Visibility.Collapsed,
            };
            main.Children.Add(Notice.Host);
            root.Children.Add(main);
        }

        // Keep the page's intro text left of the header art's busy part (the art's left ~44% is
        // plain red, then the characters start). The picture is 360 high and centred when its
        // holder is narrower than it, so where that edge lands depends on the window's size.
        const double ArtBusyFrom = 0.44;

        void FitIntro()
        {
            var intro = Ui.LastIntro;
            if (intro == null || heroImg == null || heroHolder == null || mainArea == null) return;
            var src = heroImg.Source as BitmapSource;
            if (src == null || heroHolder.ActualWidth <= 0 || intro.ActualWidth <= 0) return;
            double drawnW = 360.0 * src.PixelWidth / src.PixelHeight;          // the picture's width as drawn
            double left = (heroHolder.ActualWidth - drawnW) / 2;                // centred in its holder (<= 0 when cropped)
            if (heroHolder.ActualWidth >= drawnW) left = heroHolder.ActualWidth - drawnW;
            double busy = heroHolder.TranslatePoint(new Point(left + ArtBusyFrom * drawnW, 0), mainArea).X;
            double textLeft = intro.TranslatePoint(new Point(0, 0), mainArea).X;
            double room = busy - textLeft - 28;
            intro.MaxWidth = Math.Max(340, Math.Min(660, room));                // too narrow: a sensible minimum, the glow does the rest
        }

        static ImageSource AppIcon()
        {
            try
            {
                var st = typeof(MainWindow).Assembly.GetManifestResourceStream("icon.ico");
                return st == null ? null : BitmapFrame.Create(st);
            }
            catch { return null; }
        }

        static BitmapImage Load(string path)
        {
            var bi = new BitmapImage();
            bi.BeginInit();
            bi.CacheOption = BitmapCacheOption.OnLoad;
            bi.UriSource = new Uri(path);
            bi.EndInit();
            bi.Freeze();
            return bi;
        }

        static readonly string[][] Pages = {
            new[] { "home", "⌂", "Home" }, new[] { "mods", "▤", "Mods" }, new[] { "browse", "⬇", "Browse" }, new[] { "saves", "▣", "Saves" },
            new[] { "log", "≡", "Log" }, new[] { "settings", "⚙", "Settings" }, new[] { "about", "ⓘ", "About" },
            new[] { "build", "⚒", "Build a mod" },
        };

        void BuildNav()
        {
            nav.Children.Clear();
            foreach (var p in Pages)
            {
                if (p[0] == "build")
                {
                    var g = Ui.T("FOR MODDERS", 10.5, Theme.Faint, true);
                    g.Margin = new Thickness(10, 16, 0, 6);
                    nav.Children.Add(g);
                }
                bool on = p[0] == current;
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                var icon = Ui.T(p[1], 16, on ? Brushes.White : Theme.Muted);
                icon.Width = 26;
                row.Children.Add(icon);
                row.Children.Add(Ui.T(p[2], 15, on ? Brushes.White : Theme.Muted));
                if (p[0] == "mods" && lastOverlaps.Count > 0)
                {
                    var badge = new Border { Background = Theme.Gold, CornerRadius = new CornerRadius(9), Padding = new Thickness(7, 0, 7, 0), Margin = new Thickness(10, 2, 0, 0), Child = Ui.T(lastOverlaps.Count.ToString(), 11, Theme.B("#2a1a05"), true) };
                    row.Children.Add(badge);
                }
                var item = new Border
                {
                    Child = row, Padding = new Thickness(14, 10, 14, 10), Margin = new Thickness(0, 0, 0, 3), Cursor = Cursors.Hand,
                    BorderThickness = new Thickness(3, 0, 0, 0), CornerRadius = new CornerRadius(0, 6, 6, 0),
                    BorderBrush = on ? Theme.MenuBlueHi : Brushes.Transparent,
                };
                if (on)
                {
                    var bg = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
                    bg.GradientStops.Add(new GradientStop(Theme.C("#8C1f3fd6"), 0));
                    bg.GradientStops.Add(new GradientStop(Theme.C("#141f3fd6"), 1));
                    item.Background = bg;
                }
                else
                {
                    item.Background = Brushes.Transparent;
                    item.MouseEnter += (s, e) => item.Background = Theme.B("#1a1113");
                    item.MouseLeave += (s, e) => item.Background = Brushes.Transparent;
                }
                string id = p[0];
                item.MouseLeftButtonUp += (s, e) => Show(id);
                nav.Children.Add(item);
            }
        }

        void UpdateStatus()
        {
            bool ok = Paths.GameOk;
            statusDot.Background = ok ? Theme.Green : Theme.Error;
            statusText.Text = ok ? (Launcher.GameRunning() ? "Game running" : "Game found") : "Game not found";
        }

        public void Show(string id)
        {
            bool samePage = id == current;  // redrawn after a change: stay where the user was
            double offset = scroll.VerticalOffset;
            current = id;
            BuildNav();
            UpdateStatus();
            UIElement content;
            try
            {
                switch (id)
                {
                    case "mods": content = ModsPage(); break;
                    case "browse": content = BrowsePage(); break;
                    case "saves": content = SavesPage(); break;
                    case "log": content = LogPage(); break;
                    case "settings": content = SettingsPage(); break;
                    case "build": content = BuildPage(); break;
                    case "about": content = AboutPage(); break;
                    default: content = HomePage(); break;
                }
            }
            catch (Exception e)
            {
                content = Ui.Card(Ui.Stack(Ui.H("Something went wrong on this page"), Ui.T(e.Message, 14, Theme.Muted)));
                Log.Write("page " + id + ": " + e);
            }
            page.Child = content;
            Dispatcher.BeginInvoke(new Action(FitIntro), DispatcherPriority.Loaded);
            if (samePage) Dispatcher.BeginInvoke(new Action(() => scroll.ScrollToVerticalOffset(offset)), DispatcherPriority.Loaded);
            else scroll.ScrollToTop();
        }

        UIElement NoGame()
        {
            return Ui.Card(Ui.Stack(
                Ui.H("The game wasn't found"),
                Ui.T("Choose the Dragon Marked for Death folder (the one with game.exe) in Settings.", 14, Theme.Muted),
                Ui.Gap(16), Ui.Row(Ui.Btn("Open Settings", () => Show("settings"), true))), null, Theme.Error);
        }

        // The game isn't set up for mods yet (or a newer kit is unpacked next to this manager).
        UIElement SetupCard()
        {
            bool loader = File.Exists(Paths.LoaderDll);
            if (Kit.Here)
            {
                var mods = Kit.ModsToCopy();
                bool copyLoader = Kit.LoaderToCopy;
                if (copyLoader || mods.Count > 0)
                {
                    var what = new List<string>();
                    if (copyLoader) what.Add(loader ? "a newer mod loader (winmm.dll)" : "the mod loader (winmm.dll)");
                    what.AddRange(mods.Select(m => m.Value));
                    return Ui.Card(Ui.Stack(Ui.H(loader ? "This mod kit has updates for the game" : "Finish installing: put the mod kit into the game", 20),
                        Ui.T("This manager runs from " + Kit.Dir + ", outside the game folder, so the game doesn't have these yet: " + string.Join(", ", what) +
                             ". Installing copies them into the game folder; mods you already have keep their on/off settings.", 14, Theme.Muted),
                        Ui.Gap(12), Ui.Row(Ui.Btn("Install into the game folder", () =>
                        {
                            try { Notice.Show(Kit.Install()); CombineSoon(); }
                            catch (Exception e) { Notice.Show(e.Message, true); }
                            Show(current);
                        }, true))), null, Theme.Gold);
                }
            }
            if (!loader)
                return Ui.Card(Ui.Stack(Ui.H("The mod loader isn't installed", 20),
                    Ui.T("winmm.dll isn't in the game folder, so the game starts without mods. Copy winmm.dll and the mods folder from the mod kit's zip into the game folder, next to game.exe (or unpack the whole zip there).", 14, Theme.Muted),
                    Ui.Gap(12), Ui.Row(Ui.Btn("Open game folder", () => Open(Paths.Game), true))), null, Theme.Gold);
            if (!Directory.Exists(Paths.Mods))
                return Ui.Card(Ui.Stack(Ui.H("No mods folder yet", 20),
                    Ui.T("The game folder has no mods folder, so there's nowhere for mods to go yet.", 14, Theme.Muted),
                    Ui.Gap(12), Ui.Row(Ui.Btn("Make the mods folder", () => { Directory.CreateDirectory(Paths.Mods); Log.Write("made the mods folder"); Show(current); }, true))), null, Theme.Gold);
            return null;
        }

        // ---- Home ----
        // The loader writes mods\last_room.txt when an online room's mods didn't match ours.
        UIElement LastRoomCard()
        {
            string file = RoomCheck.FilePath;
            List<RoomMod> room;
            bool hostHasMods;
            try { if (!File.Exists(file)) return null; room = RoomCheck.Read(out hostHasMods); }
            catch (IOException) { return null; }
            var mods = ModStore.List();
            var r = RoomCheck.Compare(mods, room);
            Action dismiss = () => { try { File.Delete(file); } catch (IOException) { } Show(current); };
            var body = Ui.Stack(Ui.T("From " + File.GetLastWriteTime(file).ToString("d MMM, HH:mm") +
                (hostHasMods ? "" : ". Its host plays without mods (or with an older mod kit)") + ".", 13, Theme.Muted), Ui.Gap(6));
            if (r.Matches)
            {
                body.Children.Add(Ui.T("Your mods match that room now. Restart the game (with Launch game here) and join again.", 14, Theme.Green));
                return Ui.Card(Ui.Stack(Ui.H("Your last online room needed other mods", 20), body, Ui.Gap(12),
                    Ui.Row(Ui.Btn("Dismiss", dismiss, true))), null, Theme.Gold);
            }
            Action<string, List<string>> list = (what, items) =>
            {
                if (items.Count > 0) body.Children.Add(Ui.T(what + ": " + string.Join(", ", items), 14, Theme.Text));
            };
            list("Need to install", r.Install);
            list("Different versions", r.Versions);
            list("Need to disable", r.Disable);
            list("Need to switch on", r.SwitchOn);
            var gettable = new List<KeyValuePair<ListMod, ListVersion>>();
            var notListed = new List<string>();
            foreach (var need in r.Needed)
            {
                ListMod lm;
                var lv = ModList.Exact(need, out lm);
                if (lv != null) gettable.Add(new KeyValuePair<ListMod, ListVersion>(lm, lv));
                else notListed.Add(need.Name + " " + need.Version);
            }
            if (gettable.Count > 0)
                body.Children.Add(Ui.T("On the mod list: " + string.Join(", ", gettable.Select(g => g.Key.name + " " + g.Value.version)) + ". \"Get these mods\" installs exactly the versions the room uses.", 13, Theme.Muted));
            if (notListed.Count > 0)
                body.Children.Add(Ui.T("Not on the mod list" + (ModList.Current == null ? " (it isn't available right now)" : "") + ": " + string.Join(", ", notListed) +
                    ". Get them from their authors (or the room's host) and drop the zips on this window.", 13, Theme.Muted));
            if (!r.CanFix && gettable.Count == 0)
                body.Children.Add(Ui.T("Nothing to switch on or off yet: install or update the mods above first. Once they're installed, a button here sets everything up for this room.", 13, Theme.Muted));
            body.Children.Add(Ui.T((Launcher.GameRunning() ? "The game is still running. " : "") +
                "Steps: close the game, set up the mods here, then start the game again (Launch game) and rejoin the room.", 13, Theme.Muted));
            var buttons = Ui.Row();
            if (gettable.Count > 0)
                buttons.Children.Add(Ui.Btn("Get these mods", () => InstallFromList(gettable), true, false, "Download them from the mod list (checked) and install them"));
            if (r.CanFix)
                buttons.Children.Add(Ui.Btn("Make a profile for this room", () =>
                {
                    string name = "Room " + DateTime.Now.ToString("d MMM HH.mm");
                    string what = (r.On.Count > 0 ? "Switch on: " + string.Join(", ", r.On.Select(m => m.Name)) + ". " : "") +
                                  (r.Off.Count > 0 ? "Switch off: " + string.Join(", ", r.Off.Select(m => m.Name)) + ". " : "") +
                                  "Mods that only change your own game stay as they are. Your current setup isn't lost: save it as a profile first if it isn't one yet.";
                    if (!Dialogs.Confirm(this, "Room profile", "Set up your mods for this room?", what, "Set up for this room")) return;
                    string made = RoomCheck.MakeProfile(name, mods, r);
                    string done = made == name ? "Made the profile " + name + " and switched to it." : "Switched to your profile " + made + ", which already matches this room.";
                    Notice.Show(Launcher.GameRunning() ? done + " Close the game, then start it again with Launch game here." : done);
                    if (!Launcher.GameRunning()) CombineSoon();
                    Show(current);
                }, gettable.Count == 0));
            if (current != "mods") buttons.Children.Add(Ui.Btn("Go to Mods", () => Show("mods"), !r.CanFix && gettable.Count == 0));
            buttons.Children.Add(Ui.Btn("Dismiss", dismiss));
            return Ui.Card(Ui.Stack(Ui.H("Your last online room needed other mods", 20), body, Ui.Gap(12), buttons), null, Theme.Gold);
        }

        UIElement HomePage()
        {
            var s = Ui.Stack(Ui.PageHead("Dance with death!!", "Dragon Marked for Death",
                "Install, switch on and remove mods without touching the game's own files. Everything a mod adds sits in the mods folder, so removing it leaves the game as it was."));
            if (!Paths.GameOk) { s.Children.Add(NoGame()); return s; }
            var mods = ModStore.List();
            var setup = SetupCard();
            if (setup != null) s.Children.Add(setup);
            if (!Combiner.UpToDate(mods)) { var stale = StaleCard(); if (stale != null) s.Children.Add(stale); }
            var updates = UpdatesCard(mods);
            if (updates != null) s.Children.Add(updates);
            var room = LastRoomCard();
            if (room != null) s.Children.Add(room);

            var stats = new Grid { Margin = new Thickness(0, 0, 0, 20) };
            var backups = SaveBackups.List();
            string[][] cells = {
                new[] { "Game", "Found", Paths.FoundThrough },
                new[] { "Mods installed", mods.Count.ToString(), mods.Count(m => m.Enabled) + " switched on" },
                new[] { "Extra classes", ModStore.ExtraClasses().ToString(), "On character select" },
                new[] { "Save backups", backups.Count.ToString(), backups.Count > 0 ? "Last: " + backups[0].LastWriteTime.ToString("d MMM, HH:mm") : "None yet" },
            };
            for (int i = 0; i < cells.Length; i++)
            {
                stats.ColumnDefinitions.Add(new ColumnDefinition());
                var c = Ui.Stack(Ui.Label(cells[i][0]), Ui.T(cells[i][1], 30, Theme.Text, true, Theme.Serif), Ui.T(cells[i][2], 13, i == 0 ? Theme.Green : Theme.Muted));
                var cell = new Border { Child = c, Padding = new Thickness(24, 18, 24, 18), BorderBrush = Theme.Line, BorderThickness = new Thickness(i == 0 ? 0 : 1, 0, 0, 0) };
                Grid.SetColumn(cell, i);
                stats.Children.Add(cell);
            }
            var statBox = new Border { Child = stats, Background = new LinearGradientBrush(Theme.C("#EB1d1315"), Theme.C("#EB18142c"), 45), BorderBrush = Theme.Line, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Margin = new Thickness(0, 0, 0, 20) };
            s.Children.Add(statBox);

            var two = new Grid();
            two.ColumnDefinitions.Add(new ColumnDefinition());
            two.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(20) });
            two.ColumnDefinitions.Add(new ColumnDefinition());
            var drop = new Border
            {
                BorderBrush = Theme.B("#5a3d41"), BorderThickness = new Thickness(1.5), CornerRadius = new CornerRadius(8), Padding = new Thickness(22), Margin = new Thickness(0, 14, 0, 18),
                Child = Center(Ui.DownloadIcon(), Ui.T("Drop a mod's .zip or folder anywhere on this window.\nIt's checked before anything reaches the game.", 14, Theme.Muted)),
            };
            var quick = Ui.Card(Ui.Stack(Ui.Kicker("Quick start"), Ui.H("Add a mod"), drop,
                Ui.Row(Ui.Btn("Install from .zip...", InstallZip, true), Ui.Btn("Install from folder...", InstallFolder))));
            string loader = File.Exists(Paths.LoaderDll) ? "Installed · " + File.GetLastWriteTime(Paths.LoaderDll).ToString("d MMM yyyy") : "Not installed";
            var folders = Ui.Card(Ui.Stack(Ui.Kicker("Installation"), Ui.H("Game folders"),
                Ui.KeyValues("Game folder", Paths.Game, "Mods folder", Paths.Mods, "Mod loader", loader),
                Ui.Row(Ui.Btn("Open game folder", () => Open(Paths.Game)), Ui.Btn("Open mods folder", () => { Directory.CreateDirectory(Paths.Mods); Open(Paths.Mods); }))));
            Grid.SetColumn(folders, 2);
            two.Children.Add(quick);
            two.Children.Add(folders);
            s.Children.Add(two);
            return s;
        }

        static StackPanel Center(params UIElement[] kids)
        {
            var s = Ui.Stack(kids);
            foreach (UIElement k in s.Children)
                if (k is TextBlock t) { t.TextAlignment = TextAlignment.Center; t.HorizontalAlignment = HorizontalAlignment.Center; }
            return s;
        }

        // The data mods changed since they were combined. Normally the manager just prepares them
        // (no card): only when it can't (the game is running) or it failed is there something to say.
        UIElement StaleCard()
        {
            if (working || (combineSoon != null && combineSoon.IsEnabled)) return null;  // preparing, or about to
            if (Launcher.GameRunning())
                return Ui.Card(Ui.Stack(Ui.H("Your mods need preparing", 20),
                    Ui.T("The data mods changed, but they can't be prepared while the game is running. Close the game and they're prepared straight away.", 14, Theme.Muted)),
                    null, Theme.Gold);
            if (lastCombineProblem != null)
                return Ui.Card(Ui.Stack(Ui.H("Your mods couldn't be prepared", 20),
                    Ui.T("The game would still use the old combination. The problem is shown on the Mods page.", 14, Theme.Muted),
                    Ui.Gap(12), Ui.Row(Ui.Btn("Try again", () => Combine(null), true), Ui.Btn("Go to Mods", () => Show("mods")))), null, Theme.Error);
            CombineSoon();  // e.g. changed while the manager was closed
            return null;
        }

        // ---- Mods ----
        string filter = "", chip = "all";

        UIElement ModsPage()
        {
            var s = Ui.Stack(Ui.PageHead("Your arsenal", "Mods",
                "Switch mods on or off and set their order; changes apply the next time the game starts. Mods lower in the list win when two change the same thing."));
            if (!Paths.GameOk) { s.Children.Add(NoGame()); return s; }
            var mods = ModStore.List();

            // the loader, always on
            var loaderRow = ModRowGrid();
            AddCell(loaderRow, 0, Ui.Toggle(true, x => { }, "Always on", "", true));
            AddCell(loaderRow, 1, IconBox("◆", "Loader: starts every mod with the game (winmm.dll)"));
            AddCell(loaderRow, 2, Ui.Stack(Ui.T("DMFD Mod Loader", 16, Theme.Text, true), Ui.T(File.Exists(Paths.LoaderDll) ? "winmm.dll · " + Ui.Size(new FileInfo(Paths.LoaderDll).Length) : "winmm.dll is missing", 13, Theme.Muted)));
            AddCell(loaderRow, 3, Check(File.Exists(Paths.LoaderDll) ? "Installed" : "Missing", File.Exists(Paths.LoaderDll)));
            var head = Ui.Row(Ui.Tag("REQUIRED", Theme.Gold), Ui.Tag("ⓘ HOW IT WORKS", Theme.Blue));
            var loaderInfo = Ui.T("The mod loader starts every mod below when the game opens, combines their data in load order, and only lets you meet online players who use the same mods.", 13.5, Theme.Muted);
            s.Children.Add(Ui.Card(Ui.Stack(head, loaderInfo, Ui.Gap(8), loaderRow), new Thickness(24, 16, 24, 8), Theme.B("#80e6b84e")));

            var setup = SetupCard();
            if (setup != null) s.Children.Add(setup);
            if (!Combiner.UpToDate(mods)) { var stale = StaleCard(); if (stale != null) s.Children.Add(stale); }
            var roomCard = LastRoomCard();
            if (roomCard != null) s.Children.Add(roomCard);
            if (lastCombineProblem != null)
                s.Children.Add(Ui.Card(Ui.Stack(Ui.H("The mods couldn't be combined", 20), Ui.T(lastCombineProblem, 13, Theme.Muted, false, Theme.Mono)), null, Theme.Error));
            if (lastOverlaps.Count > 0)
            {
                var ov = Ui.Stack(Ui.H("Overlaps", 20), Ui.T("These mods change the same things. The one lower in the list is used; move a mod to change which.", 13.5, Theme.Muted), Ui.Gap(8));
                foreach (string o in lastOverlaps.Take(30)) ov.Children.Add(Ui.T("•  " + o, 13.5, Theme.Text));
                if (lastOverlaps.Count > 30) ov.Children.Add(Ui.T("... and " + (lastOverlaps.Count - 30) + " more", 13, Theme.Muted));
                s.Children.Add(Ui.Card(ov, null, Theme.Gold));
            }

            // profiles: saved setups to switch between
            var profRow = Ui.Row();
            string currentProfile = Profiles.Current(mods);
            foreach (string pn in Profiles.Names())
            {
                string name = pn;
                bool active = name == currentProfile;
                bool twin = !active && currentProfile != null && Profiles.Matches(name, mods);  // same setup as the current one
                var chip = Ui.Chip((active ? "✓  " : "") + name, active, () =>
                {
                    if (Launcher.GameRunning()) throw new Exception("Close the game first.");
                    var missing = Profiles.Apply(name, ModStore.List());
                    Notice.Show(missing.Count == 0 ? "Switched to " + name + "." : "Switched to " + name + ", but these mods aren't installed: " + string.Join(", ", missing), missing.Count > 0);
                    CombineSoon();
                    Show("mods");
                }, () =>
                {
                    if (!Dialogs.Confirm(this, "Delete profile", "Delete the profile " + name + "?", "Only the saved setup goes; the mods themselves stay installed.", "Delete profile", true)) return;
                    Profiles.Delete(name);
                    Show("mods");
                }, active ? "This is your setup right now." : twin ? "Same setup as " + currentProfile + " (switching to it changes nothing but the name shown)."
                               : "Switch to " + name + ": its mods on, in its order, every other mod off.", "Delete the profile " + name);
                profRow.Children.Add(chip);
            }
            profRow.Children.Add(Ui.Btn("Save current as...", () =>
            {
                string same = Profiles.Names().FirstOrDefault(n => Profiles.Matches(n, ModStore.List()));
                if (same != null && !Dialogs.Confirm(this, "Save profile", "This setup is already saved as " + same + ".", "Save another copy under a new name anyway?", "Save a copy")) return;
                string name = Dialogs.Ask(this, "Save this setup as a profile", "Name it, for example Solo, or the friend whose room it matches:", "Save profile");
                if (string.IsNullOrWhiteSpace(name)) return;
                Profiles.Save(name.Trim(), ModStore.List());
                Notice.Show("Saved the profile " + name.Trim() + ".");
                Show("mods");
            }, Profiles.Names().Count == 0));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Profiles", 20),
                Ui.T("Saved setups to switch between: one for solo, one matching a friend's room, and so on. Switching turns the profile's mods on, in its order, and every other mod off.", 13.5, Theme.Muted),
                Ui.Gap(12), profRow), new Thickness(26, 20, 26, 10)));

            // toolbar
            var tools = new DockPanel { Margin = new Thickness(0, 4, 0, 12) };
            var buttons = Ui.Row(Ui.Btn("⟳ Refresh", () => Show("mods")), Ui.Btn("Test without mods", () => Launch(true), false, false, "Starts the game once with every mod switched off"));
            buttons.Margin = new Thickness(10, 0, 0, 0);
            DockPanel.SetDock(buttons, Dock.Right);
            tools.Children.Add(buttons);
            var box = Ui.Input(filter, "Filter: mod name, author or type");
            var boxHost = Ui.WithHint(box, "Filter: mod name, author or type");
            boxHost.Margin = new Thickness(0, 0, 0, 10);
            tools.Children.Add(boxHost);
            s.Children.Add(tools);
            var chips = Ui.Tabs(new[] { new[] { "all", "All (" + mods.Count + ")" }, new[] { "on", "On (" + mods.Count(m => m.Enabled) + ")" }, new[] { "off", "Off (" + mods.Count(m => !m.Enabled) + ")" } },
                                chip, key => { chip = key; Show("mods"); }, true);

            var table = new StackPanel();
            Grid.SetIsSharedSizeScope(table, true);  // the check and tools columns line up across rows
            var header = ModRowGrid();
            string[] heads = { "STATUS", "TYPE", "MOD", "CHECK", "ORDER & TOOLS" };
            for (int i = 0; i < heads.Length; i++)
            {
                var lab = Ui.Label(heads[i]);
                AddCell(header, i, lab);
                if (i == 4) { lab.HorizontalAlignment = HorizontalAlignment.Right; lab.TextAlignment = TextAlignment.Right; }
                if (i == 1) lab.Margin = new Thickness(10, 0, 6, 0);  // over the icons
            }
            header.Margin = new Thickness(22, 12, 22, 8);
            table.Children.Add(header);
            var rows = new StackPanel();
            var rowsHost = new Grid();
            rowsHost.Children.Add(rows);
            table.Children.Add(rowsHost);
            DropLine(rowsHost, rows, mods);
            Action fill = () =>
            {
                rows.Children.Clear();
                int shown = 0;
                for (int i = 0; i < mods.Count; i++)
                {
                    var m = mods[i];
                    string hay = (m.Name + " " + m.Author + " " + m.Type + " " + m.Folder).ToLowerInvariant();
                    if (filter != "" && !hay.Contains(filter.ToLowerInvariant())) continue;
                    if (chip == "on" && !m.Enabled || chip == "off" && m.Enabled) continue;
                    rows.Children.Add(ModRow(mods, i));
                    shown++;
                }
                if (shown == 0)
                    rows.Children.Add(new Border { Padding = new Thickness(30), Child = Center(Ui.T(mods.Count == 0 ? "No mods installed yet. Drop a mod's .zip on this window to add one." : "No mod matches the filter.", 14, Theme.Muted)) });
            };
            fill();
            box.TextChanged += (o, e) => { filter = box.Text; fill(); };
            s.Children.Add(Ui.TabbedCard(chips, table));
            return s;
        }

        static Grid ModRowGrid()
        {
            // status and type: fixed; mod: the rest, never less than 220; check and tools: as wide as
            // what's in them, the same in every row of a list (shared sizes, see ModsPage)
            var g = new Grid { Margin = new Thickness(22, 10, 22, 10) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(124) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(74) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star), MinWidth = 220 });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, SharedSizeGroup = "Check" });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, SharedSizeGroup = "Tools" });
            return g;
        }

        static void AddCell(Grid g, int col, UIElement e)
        {
            if (e is FrameworkElement fe) { fe.VerticalAlignment = VerticalAlignment.Center; if (col > 0) fe.Margin = new Thickness(6, 0, 6, 0); }
            Grid.SetColumn(e, col);
            g.Children.Add(e);
        }

        static Border IconBox(string glyph, string tip = null)
        {
            var t = Ui.T(glyph, 20, Theme.Gold);
            t.HorizontalAlignment = HorizontalAlignment.Center;
            t.VerticalAlignment = VerticalAlignment.Center;
            return new Border { Width = 46, Height = 46, CornerRadius = new CornerRadius(5), BorderBrush = Theme.Line, BorderThickness = new Thickness(1), Background = Theme.B("#2a1c1f"), Child = t, HorizontalAlignment = HorizontalAlignment.Left, ToolTip = tip };
        }

        static StackPanel Check(string text, bool ok, bool warn = false)
        {
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            sp.Children.Add(Ui.T(ok ? "✓ " : warn ? "⚠ " : "✕ ", 13.5, ok ? Theme.Green : warn ? Theme.Gold : Theme.Error));
            sp.Children.Add(Ui.T(text, 13.5, ok ? Theme.Green : warn ? Theme.Gold : Theme.Error));
            return sp;
        }

        UIElement ModRow(List<ModInfo> mods, int i)
        {
            var m = mods[i];
            var g = ModRowGrid();
            g.Margin = new Thickness(22, 12, 22, 12);
            g.Opacity = m.Enabled ? 1 : 0.6;
            var row = new Border { Child = g, BorderBrush = Theme.LineSoft, BorderThickness = new Thickness(0, 1, 0, 0), Background = Brushes.Transparent, Tag = i };
            AddCell(g, 0, Ui.Toggle(m.Enabled, on =>
            {
                ModStore.SetEnabled(m, on);
                g.Opacity = on ? 1 : 0.6;
                Notice.Show(m.Name + " switched " + (on ? "on" : "off") + ". Applies the next time the game starts.");
                Log.Write(m.Name + " switched " + (on ? "on" : "off"));
                if (m.HasData) CombineSoon();
            }));
            AddCell(g, 1, IconBox(m.Dll != "" ? "‹›" : m.HasData ? "✦" : "□",
                m.Dll != "" && m.HasData ? "Code and data: changes how the game works, and its content"
                : m.Dll != "" ? "Code: changes how the game works (" + m.Dll + ")"
                : m.HasData ? "Data: adds or changes content (classes, items, text, art)"
                : "Empty: this mod has nothing in it yet"));
            string meta = "Version " + m.Version + (m.Author != "" ? " · by " + m.Author : "") + (m.License != "" ? " · " + m.License : "") + " · " + m.FileCount + " file(s) · " + Ui.Size(m.SizeBytes);
            var nameLine = Ui.Row(Ui.T(m.Name, 16, Theme.Text, true));
            if (RoomCheck.AnyRoom(m))
            {
                var tag = Ui.Tag("ANY ROOM", Theme.Green);
                tag.Margin = new Thickness(10, 0, 0, 0);
                tag.ToolTip = "Only changes your own game: you can play online in any room with it, modded or not.";
                nameLine.Children.Add(tag);
            }
            var nameBlock = Ui.Stack(nameLine, Ui.T(meta, 13, Theme.Muted));
            if (m.Description != "") { var d = Ui.T(m.Description, 12.5, Theme.Faint); d.Margin = new Thickness(0, 3, 0, 0); nameBlock.Children.Add(d); }
            AddCell(g, 2, nameBlock);
            bool overlap = lastOverlaps.Any(o => o.Contains(m.Name));
            AddCell(g, 3, m.DllMissing ? Check(m.Dll + " is missing", false) : !m.Enabled ? Check("Off", true) : overlap ? Check("Overlaps", false, true) : Check("Ready", true));
            var acts = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            acts.Children.Add(Ui.Btn("↑", () => Move(mods, i, -1), false, true, "Load earlier"));
            acts.Children.Add(Ui.Btn("↓", () => Move(mods, i, +1), false, true, "Load later"));
            acts.Children.Add(Ui.Btn("Folder", () => Open(m.Dir), false, true, "Open mods\\" + m.Folder));
            acts.Children.Add(Ui.Btn("Remove", () => RemoveMod(m), false, true, "Moves the mod's folder to the Recycle Bin"));
            foreach (FrameworkElement a in acts.Children) { a.Margin = new Thickness(4, 0, 0, 0); a.VerticalAlignment = VerticalAlignment.Center; }
            var grip = Grip(row, mods, i);
            grip.Margin = new Thickness(14, 0, -10, 0);  // at the very right edge
            grip.VerticalAlignment = VerticalAlignment.Center;
            acts.Children.Add(grip);
            AddCell(g, 4, acts);
            return row;
        }

        // ---- drag a mod by its grip to change the load order ----
        const string DragFormat = "dmfd-mod-order";

        FrameworkElement Grip(Border row, List<ModInfo> mods, int i)
        {
            bool can = chip == "all" && filter == "";  // the whole list is showing: a drop place means something
            var t = Ui.T("⋮⋮", 16, can ? Theme.Muted : Theme.B("#4d3d38"));
            t.TextWrapping = TextWrapping.NoWrap;
            t.HorizontalAlignment = HorizontalAlignment.Center;
            var grip = new Border
            {
                Child = t, Background = Brushes.Transparent,
                Cursor = can ? Cursors.SizeAll : Cursors.Arrow, Padding = new Thickness(6, 10, 6, 10),
                ToolTip = can ? "Drag to change the load order (lower in the list wins)" : "Show All, with no filter, to drag mods into a new order",
            };
            if (!can) return grip;
            Point start = default(Point);
            bool pressed = false;
            grip.MouseEnter += (s, e) => t.Foreground = Theme.Gold;
            grip.MouseLeave += (s, e) => t.Foreground = Theme.Muted;
            grip.MouseLeftButtonDown += (s, e) => { pressed = true; start = e.GetPosition(this); e.Handled = true; };
            grip.MouseLeftButtonUp += (s, e) => pressed = false;
            grip.MouseMove += (s, e) =>
            {
                if (!pressed || e.LeftButton != MouseButtonState.Pressed) { pressed = false; return; }
                var p = e.GetPosition(this);
                if (Math.Abs(p.Y - start.Y) < 4 && Math.Abs(p.X - start.X) < 4) return;
                pressed = false;
                row.Opacity = 0.45;
                DragDrop.DoDragDrop(grip, new DataObject(DragFormat, mods[i].Folder), DragDropEffects.Move);
                row.Opacity = 1;
            };
            return grip;
        }

        // While dragging over the list: one blue line, arrowheads at both ends, between the two
        // rows where the mod would land (none where it would land in its own place). Dropping
        // there moves it.
        void DropLine(Grid host, StackPanel rows, List<ModInfo> mods)
        {
            var line = new System.Windows.Shapes.Path
            {
                Fill = Theme.MenuBlueHi, IsHitTestVisible = false, Visibility = Visibility.Collapsed,
            };
            var canvas = new Canvas { IsHitTestVisible = false };
            canvas.Children.Add(line);
            host.Children.Add(canvas);
            host.AllowDrop = true;
            host.Background = Brushes.Transparent;  // so the gaps between rows take drops too
            int target = -1;
            Func<DragEventArgs, int> place = e =>
            {
                double y = e.GetPosition(rows).Y;
                int last = -1;
                foreach (UIElement c in rows.Children)
                {
                    var fe = c as FrameworkElement;
                    if (fe == null || !(fe.Tag is int)) continue;
                    double top = fe.TranslatePoint(new Point(0, 0), rows).Y;
                    if (y < top + fe.ActualHeight / 2) return (int)fe.Tag;
                    last = (int)fe.Tag;
                }
                return last + 1;
            };
            Action hide = () => { line.Visibility = Visibility.Collapsed; target = -1; };
            host.DragOver += (s, e) =>
            {
                if (!e.Data.GetDataPresent(DragFormat)) return;
                e.Handled = true;
                int from = mods.FindIndex(x => x.Folder == (string)e.Data.GetData(DragFormat));
                int to = place(e);
                if (from < 0 || to == from || to == from + 1) { e.Effects = DragDropEffects.None; hide(); return; }
                e.Effects = DragDropEffects.Move;
                if (to == target) return;
                target = to;
                // the boundary above row "to" (or below the last row)
                double y = 0;
                foreach (UIElement c in rows.Children)
                {
                    var fe = c as FrameworkElement;
                    if (fe == null || !(fe.Tag is int)) continue;
                    double top = fe.TranslatePoint(new Point(0, 0), rows).Y;
                    if ((int)fe.Tag == to) { y = top; break; }
                    y = top + fe.ActualHeight;
                }
                double w = rows.ActualWidth, a = 7;
                a = 9;  // triangle length; half its height is 6
                // one outline (no seams): each end's triangle narrows straight into the bar
                line.Data = Geometry.Parse(string.Format(System.Globalization.CultureInfo.InvariantCulture,
                    "M 0,{0} L {1},{2} H {3} L {4},{0} V {5} L {3},{6} H {1} L 0,{5} Z",
                    y - 6, a, y - 1.5, w - a, w, y + 6, y + 1.5));
                line.Visibility = Visibility.Visible;
            };
            host.DragLeave += (s, e) =>
            {
                var p = e.GetPosition(host);
                if (p.X < 0 || p.Y < 0 || p.X >= host.ActualWidth || p.Y >= host.ActualHeight) hide();
            };
            host.Drop += (s, e) =>
            {
                if (!e.Data.GetDataPresent(DragFormat)) return;
                e.Handled = true;
                int to = target;
                hide();
                int from = mods.FindIndex(x => x.Folder == (string)e.Data.GetData(DragFormat));
                if (from < 0 || to < 0 || to == from || to == from + 1) return;
                var moved = mods[from];
                mods.RemoveAt(from);
                if (to > from) to--;
                mods.Insert(to, moved);
                ModStore.SaveOrder(mods);
                Log.Write("moved " + moved.Name + " to place " + (to + 1));
                Notice.Show(moved.Name + " now loads " + (to == 0 ? "first." : to == mods.Count - 1 ? "last." : "after " + mods[to - 1].Name + "."));
                if (mods.Any(x => x.HasData && x.Enabled)) CombineSoon();
                Dispatcher.BeginInvoke(new Action(() => Show("mods")));  // after the drag has finished
            };
        }

        void Move(List<ModInfo> mods, int i, int by)
        {
            int j = i + by;
            if (j < 0 || j >= mods.Count) return;
            var t = mods[i];
            mods[i] = mods[j];
            mods[j] = t;
            ModStore.SaveOrder(mods);
            Log.Write("moved " + t.Name + (by < 0 ? " earlier" : " later"));
            if (mods[i].HasData && mods[i].Enabled || mods[j].HasData && mods[j].Enabled) CombineSoon();
            Show("mods");
        }

        void RemoveMod(ModInfo m)
        {
            if (Launcher.GameRunning()) throw new Exception("Close the game first.");
            if (!Dialogs.Confirm(this, "Remove mod", "Remove " + m.Name + "?", "Its folder goes to the Recycle Bin, so you can get it back from there.", "Remove", true)) return;
            bool data = m.HasData && m.Enabled;
            ModStore.Remove(m);
            Notice.Show(m.Name + " removed (it's in the Recycle Bin).");
            if (data) CombineSoon();
            Show("mods");
        }

        // ---- installing ----
        void InstallZip()
        {
            var d = new Microsoft.Win32.OpenFileDialog { Filter = "Mod zip (*.zip)|*.zip", Title = "Install a mod" };
            if (d.ShowDialog(this) == true) Install(new[] { d.FileName });
        }

        void InstallFolder()
        {
            using (var d = new System.Windows.Forms.FolderBrowserDialog { Description = "Choose the mod's folder (the one with mod.ini)" })
                if (d.ShowDialog() == System.Windows.Forms.DialogResult.OK) Install(new[] { d.SelectedPath });
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop)) Install((string[])e.Data.GetData(DataFormats.FileDrop));
        }

        void Install(string[] paths)
        {
            if (!Paths.GameOk) { Notice.Show("Find the game first (Settings).", true); return; }
            if (Launcher.GameRunning()) { Notice.Show("Close the game first, then install.", true); return; }
            var done = new List<string>();
            foreach (string p in paths)
            {
                try { done.Add(ModStore.Install(p)); }
                catch (Exception ex) { Notice.Show(Path.GetFileName(p) + ": " + ex.Message, true); return; }
            }
            if (done.Count == 0) return;
            Notice.Show("Installed " + string.Join(", ", done) + ".");
            Show("mods");
            CombineSoon();
        }

        // ---- combining ----
        void CombineSoon()
        {
            if (combineSoon == null)
            {
                combineSoon = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
                combineSoon.Tick += (s, e) => { combineSoon.Stop(); if (!Combiner.UpToDate(ModStore.List())) Combine(null); };
            }
            combineSoon.Stop();
            combineSoon.Start();
        }

        // Combines in the background behind "Preparing mods..."; then runs next (if any).
        void Combine(Action next)
        {
            if (next != null) afterCombine = next;
            if (working) { combineAgain = true; return; }  // once this one's done
            if (Launcher.GameRunning()) { Notice.Show("Close the game first: mods are prepared while it isn't running.", true); afterCombine = null; return; }
            working = true;
            combineAgain = false;
            busyTitle.Text = afterCombine != null ? "Preparing mods, then starting the game..." : "Preparing mods...";
            busyLine.Text = "This takes a few seconds; you can keep using the manager.";
            busy.Visibility = Visibility.Visible;
            var started = DateTime.Now;
            // the bar fills over about as long as the last time took (it can't know more than that)
            double expect;
            if (!double.TryParse(Ini.Get(Paths.SettingsFile, "combine", "last_seconds", "5"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out expect) || expect < 0.5)
                expect = 5;
            busyBar.Value = 0;
            if (busyTick == null) busyTick = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
            busyTick.Tick -= OnBusyTick;
            busyTick.Tag = Tuple.Create(started, expect);
            busyTick.Tick += OnBusyTick;
            busyTick.Start();
            var problems = new List<string>();
            new Thread(() =>
            {
                List<string> overlaps;
                bool ok = Combiner.Run(line =>
                {
                    if (line.StartsWith("  - ")) lock (problems) problems.Add(line.Substring(4));
                    string shown = Stage(line);
                    if (shown != null) Dispatcher.BeginInvoke(new Action(() => busyLine.Text = shown + "  (" + (int)(DateTime.Now - started).TotalSeconds + " s)"));
                }, out overlaps);
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    working = false;
                    busyTick.Stop();
                    busyBar.Value = 100;
                    busy.Visibility = Visibility.Collapsed;
                    if (ok) Ini.Set(Paths.SettingsFile, "combine", "last_seconds", (DateTime.Now - started).TotalSeconds.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                    lastOverlaps = overlaps;
                    var then = afterCombine;
                    afterCombine = null;
                    lastCombineProblem = ok ? null : (problems.Count > 0 ? string.Join("\n", problems) : busyLine.Text);
                    Notice.Show(ok ? (overlaps.Count > 0 ? "Mods prepared, with " + overlaps.Count + " overlap(s): see the Mods page." : "Mods prepared.")
                                   : "The mods couldn't be combined: see the Mods page.", !ok);
                    Refresh();
                    if (combineAgain && !Combiner.UpToDate(ModStore.List())) Combine(then);  // changes made meanwhile
                    else if (ok && then != null) then();
                }));
            }) { IsBackground = true }.Start();
        }

        void OnBusyTick(object sender, EventArgs e)
        {
            var t = (Tuple<DateTime, double>)busyTick.Tag;
            double secs = (DateTime.Now - t.Item1).TotalSeconds;
            busyBar.Value = Math.Min(97, 100 * secs / t.Item2);
        }

        // The combiner's output, in words for the progress panel (null = keep what's shown).
        static string Stage(string line)
        {
            string t = line.Trim();
            if (t.StartsWith("Layout")) return "Applying layout changes";
            if (t.StartsWith("note: sprites: atlas")) return "Adding class pictures to the game's art sheets";
            if (t.StartsWith("Portraits")) return "Portraits done";
            if (t.StartsWith("HUD faces")) return "Quest faces done";
            if (t.StartsWith("HUD life frames")) return "Life frames done";
            if (t.StartsWith("Select screen art")) return "Character select art done; writing the game files";
            if (t.StartsWith("Combined")) return "Done";
            return null;
        }

        // Asked to show (a second start, or the game's menu): to the front, on Home (room card etc.).
        public void BringToFront()
        {
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            Show("home");
            Activate();
            Topmost = true;   // Windows only lets a window jump in front like this
            Topmost = false;
            Focus();
        }

        // Redraw the page, unless the user is typing in it (it would lose their place).
        void Refresh()
        {
            if (Keyboard.FocusedElement is TextBox) { BuildNav(); return; }
            Show(current);
        }

        // ---- launching ----
        void Launch(bool withoutMods)
        {
            if (!Paths.GameOk) { Notice.Show("Find the game first (Settings).", true); return; }
            if (Launcher.GameRunning()) { Notice.Show("The game is already running.", true); return; }
            Action go = () => Ui.Guard(() =>
            {
                if (SaveBackups.AutoBackup && SaveBackups.SaveFiles().Length > 0) SaveBackups.Make("before launch");
                Launcher.Start(withoutMods);
                Notice.Show(withoutMods ? "Starting the game once without mods..." : "Starting the game through Steam...");
                Show(current);
            });
            if (!withoutMods && (working || !Combiner.UpToDate(ModStore.List())))
            {
                Notice.Show("The game starts as soon as the mods are prepared.");
                Combine(go);
            }
            else go();
        }

        // ---- Saves ----
        UIElement SavesPage()
        {
            var s = Ui.Stack(Ui.PageHead("Safekeeping", "Saves",
                "A broken mod can break a save. Before each launch from here, the manager copies your save and the extra classes' save."));
            if (Paths.Saves == "" || !Directory.Exists(Paths.Saves))
            {
                s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Your save wasn't found"), Ui.T("Choose the folder with SystemData.bin in Settings.", 14, Theme.Muted), Ui.Gap(14), Ui.Row(Ui.Btn("Open Settings", () => Show("settings"), true))), null, Theme.Error));
                return s;
            }
            var keep = Ui.Input(SaveBackups.Keep.ToString(), "How many backups to keep", 64);
            keep.LostFocus += (o, e) => { int n; if (int.TryParse(keep.Text, out n) && n > 0) { SaveBackups.Keep = n; Notice.Show("Keeping the last " + SaveBackups.Keep + " backups."); } keep.Text = SaveBackups.Keep.ToString(); };
            var keepRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 4, 0, 16) };
            keepRow.Children.Add(Ui.Toggle(SaveBackups.AutoBackup, on => SaveBackups.AutoBackup = on, "Back up my save every time I launch the game here", "Back up my save every time I launch the game here"));
            var k = Ui.T("Keep the last", 14, Theme.Text);
            k.Margin = new Thickness(36, 0, 10, 0);
            k.VerticalAlignment = VerticalAlignment.Center;
            keepRow.Children.Add(k);
            keepRow.Children.Add(keep);
            var k2 = Ui.T("backups (older ones are deleted)", 14, Theme.Faint);
            k2.Margin = new Thickness(10, 0, 0, 0);
            k2.VerticalAlignment = VerticalAlignment.Center;
            keepRow.Children.Add(k2);

            var list = Ui.Stack(Ui.H("Backups"), Ui.T("To go back to one, close the game and press Restore. The save you have now is backed up first, so a restore can be undone.", 14, Theme.Muted), Ui.Gap(14), keepRow);
            var backups = SaveBackups.List();
            if (backups.Count == 0) list.Children.Add(Ui.T("No backups yet.", 14, Theme.Faint));
            foreach (var b in backups)
            {
                var g = new Grid { Margin = new Thickness(0, 12, 0, 12) };
                g.ColumnDefinitions.Add(new ColumnDefinition());
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(160) });
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                string why = Path.GetFileNameWithoutExtension(b.Name);
                why = why.Length > 18 ? why.Substring(18) : "";
                AddCell(g, 0, Ui.Stack(Ui.T(b.LastWriteTime.ToString("dddd d MMM, HH:mm"), 15, Theme.Text, true), Ui.T(why == "" ? "" : char.ToUpper(why[0]) + why.Substring(1), 13, Theme.Muted)));
                AddCell(g, 1, Ui.T(Ui.Size(b.Length), 13, Theme.Muted, false, Theme.Mono));
                var bb = b;
                AddCell(g, 2, Ui.Btn("Restore", () =>
                {
                    if (!Dialogs.Confirm(this, "Restore save", "Restore the save from " + bb.LastWriteTime.ToString("d MMM, HH:mm") + "?", "Your save as it is now is backed up first, so this can be undone.", "Restore")) return;
                    SaveBackups.Restore(bb);
                    Notice.Show("Save restored. (The one you had is in the backups too.)");
                    Show("saves");
                }, false, true));
                list.Children.Add(new Border { Child = g, BorderBrush = Theme.LineSoft, BorderThickness = new Thickness(0, 1, 0, 0) });
            }
            s.Children.Add(Ui.Card(list));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Where your save is"),
                Ui.KeyValues("Save folder", Paths.Saves, "Game save", "SystemData.bin", "Extra classes", "SystemData.bin.classes"),
                Ui.Row(Ui.Btn("Back up now", () => { SaveBackups.Make("by hand"); Notice.Show("Save backed up."); Show("saves"); }, true),
                       Ui.Btn("Open save folder", () => Open(Paths.Saves)),
                       Ui.Btn("Open backups", () => { Directory.CreateDirectory(Paths.Backups); Open(Paths.Backups); })))));
            return s;
        }

        // ---- Log ----
        string logTab = null, logSearch = "";
        bool onlyProblems;

        UIElement LogPage()
        {
            var s = Ui.Stack(Ui.PageHead("Battle report", "Log",
                "What the loader and each mod wrote during the last game. If something goes wrong, \"Report a problem\" gathers these into one file to send."));
            if (!Paths.GameOk) { s.Children.Add(NoGame()); return s; }
            var logs = Log.GameLogs();
            if (logs.Count == 0) { s.Children.Add(Ui.Card(Ui.T("No logs yet: they appear after the game has run with the mod loader.", 14, Theme.Muted))); return s; }
            if (logTab == null || !logs.Any(l => l.Key == logTab)) logTab = logs[0].Key;
            string path = logs.First(l => l.Key == logTab).Value;
            string[] lines = Log.ReadShared(path);

            int errors = lines.Count(l => l.Contains("ERROR") || l.Contains("CRASH") || l.Contains("failed")), warnings = lines.Count(l => l.Contains("WARNING"));
            string started = lines.Length > 0 && lines[0].Length >= 8 ? File.GetLastWriteTime(path).ToString("d MMM, HH:mm") : "";
            var sum = Ui.T(logTab + " log, last written " + started + ": " + errors + " problem line(s), " + warnings + " warning(s).", 14, errors > 0 ? Theme.Warn : Theme.Text);

            var tabs = Ui.Tabs(logs.Select(l => new[] { l.Key, l.Key }).ToArray(), logTab, key => { logTab = key; Show("log"); }, false);
            var search = Ui.Input(logSearch, "Search the log", 300);
            var searchHost = Ui.WithHint(search, "Search the log, e.g. a mod's name");
            var only = Ui.Toggle(onlyProblems, on => { onlyProblems = on; Show("log"); }, "Only problems and warnings", "Only problems and warnings");
            var text = new TextBox
            {
                IsReadOnly = true, FontFamily = Theme.Mono, FontSize = 13, Foreground = Theme.B("#d8cbbd"), Background = Theme.B("#66000000"),
                BorderBrush = Theme.Line, Padding = new Thickness(14), Height = 420, TextWrapping = TextWrapping.Wrap,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            };
            Action fill = () =>
            {
                var shown = lines.Where(l => (logSearch == "" || l.IndexOf(logSearch, StringComparison.OrdinalIgnoreCase) >= 0) &&
                                             (!onlyProblems || l.Contains("ERROR") || l.Contains("WARNING") || l.Contains("CRASH") || l.Contains("failed"))).ToList();
                if (shown.Count > 3000) shown = shown.Skip(shown.Count - 3000).ToList();
                text.Text = shown.Count > 0 ? string.Join("\n", shown)
                          : onlyProblems && logSearch == "" ? "No problems and warnings detected." : "Nothing matches.";
                text.ScrollToEnd();
            };
            fill();
            search.TextChanged += (o, e) => { logSearch = search.Text; fill(); };
            var toolbar = new DockPanel { Margin = new Thickness(0, 6, 0, 10) };
            DockPanel.SetDock(searchHost, Dock.Right);
            toolbar.Children.Add(searchHost);
            toolbar.Children.Add(only);
            s.Children.Add(Ui.Card(Ui.Stack(sum, Ui.Gap(14),
                Ui.Row(Ui.Btn("⟳ Refresh", () => Show("log"), false, true), Ui.Btn("Copy", () => { Clipboard.SetText(text.Text); Notice.Show("Copied."); }, false, true),
                       Ui.Btn("Open log file", () => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }), false, true),
                       Ui.Btn("Report a problem...", () => { string z = Log.Report(); Notice.Show("Made " + Path.GetFileName(z) + ": send that file to whoever is helping you."); Process.Start("explorer.exe", "/select,\"" + z + "\""); }, true, true)),
                tabs, toolbar, text)));
            return s;
        }

        // ---- Settings ----
        UIElement SettingsPage()
        {
            var s = Ui.Stack(Ui.PageHead("Configuration", "Settings", null));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Game installation"),
                Ui.T("Found automatically through Steam. Choose the folder yourself if it wasn't found, or to use another copy of the game.", 14, Theme.Muted),
                Ui.KeyValues("Game folder", Paths.GameOk ? Paths.Game : "not found", "Found through", Paths.GameOk ? Paths.FoundThrough : "-"),
                Ui.Row(Ui.Btn("Locate game folder...", () =>
                {
                    using (var d = new System.Windows.Forms.FolderBrowserDialog { Description = "Choose the Dragon Marked for Death folder (with game.exe)" })
                    {
                        if (d.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;
                        if (!File.Exists(Path.Combine(d.SelectedPath, "game.exe"))) throw new Exception("That folder has no game.exe.");
                        Paths.Game = d.SelectedPath;
                        Paths.FoundThrough = "Chosen in Settings";
                        Ini.Set(Paths.SettingsFile, "paths", "game", Paths.Game);
                        Show("settings");
                    }
                }, true), Ui.Btn("Search again", () => { Ini.Set(Paths.SettingsFile, "paths", "game", ""); Paths.Game = ""; Paths.Find(null, null); Show("settings"); })))));

            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Save folder"),
                Ui.T("Where SystemData.bin is. Found through Steam; choose it yourself if Steam keeps its user data somewhere else.", 14, Theme.Muted),
                Ui.KeyValues("Save folder", Paths.Saves != "" ? Paths.Saves : "not found"),
                Ui.Row(Ui.Btn("Choose save folder...", () =>
                {
                    using (var d = new System.Windows.Forms.FolderBrowserDialog { Description = "Choose the folder with SystemData.bin" })
                    {
                        if (d.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;
                        if (!File.Exists(Path.Combine(d.SelectedPath, "SystemData.bin"))) throw new Exception("That folder has no SystemData.bin.");
                        Paths.Saves = d.SelectedPath;
                        Ini.Set(Paths.SettingsFile, "paths", "saves", Paths.Saves);
                        Show("settings");
                    }
                }), Ui.Btn("Open save folder", () => Open(Paths.Saves))))));

            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Online play"),
                Ui.T("With mods that matter online (data mods, and code mods like Rebalance that change the game for everyone), you see other modded rooms and no vanilla ones; vanilla players don't see yours. Mods that only change your own game, like Quick Spells, don't count.", 14, Theme.Muted),
                Ui.Gap(10),
                Ui.T("Joining a room whose mods differ from yours: the game stops you at the door and says what to install, disable or update. The Home page here then offers to make a profile for that room. Install any mods you're missing, switch to the profile, restart the game, and join again.", 14, Theme.Muted),
                Ui.Gap(12),
                Ui.Row(Ui.Toggle(RoomCheck.ModdedRooms, on => { RoomCheck.ModdedRooms = on; Notice.Show(on ? "You'll see modded rooms from the next game start." : "Back to the rooms that fit your mods, from the next game start."); },
                                 "Look for modded rooms", "Look for modded rooms")),
                Ui.T("For players whose mods all work in any room (marked ANY ROOM): see modded rooms instead of vanilla ones. Joining one tells you which mods it needs. With mods that must match online, you see modded rooms anyway.", 13, Theme.Faint),
                Ui.Gap(10),
                Ui.T("Tip: save your own setup as a profile on the Mods page before changing it, and another for each group you play with. Switching back is then one click.", 14, Theme.Muted))));

            string src = ModList.Source;
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Mod list"),
                Ui.T("Where the Browse page gets the approved mod list from. It comes with the mod kit's releases; a file or web address can be set here (for testing a list, say).", 14, Theme.Muted),
                Ui.KeyValues("Mod list", src == "" ? "not set up yet" : src),
                Ui.Row(Ui.Btn("Use a file...", () =>
                       {
                           var d = new Microsoft.Win32.OpenFileDialog { Filter = "Mod list (mods.json)|*.json", Title = "The mod list (mods.json)" };
                           if (d.ShowDialog(this) != true) return;
                           ModList.Source = d.FileName;
                           ModList.Current = null;
                           listTried = false;
                           Show("settings");
                       }),
                       Ui.Btn("Use a web address...", () =>
                       {
                           string url = Dialogs.Ask(this, "Mod list", "The mod list's web address (https://...):", "Use it");
                           if (string.IsNullOrWhiteSpace(url)) return;
                           ModList.Source = url.Trim();
                           ModList.Current = null;
                           listTried = false;
                           Show("settings");
                       }),
                       Ui.Btn("Default", () => { ModList.Source = ""; ModList.Current = null; listTried = false; Show("settings"); }, false, false, "The list that comes with the mod kit")))));

            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Manager data"),
                Ui.T("Settings, save backups, problem reports and the manager's own log, kept in the ModManager folder next to the game.", 14, Theme.Muted),
                Ui.KeyValues("Data folder", Paths.Data),
                Ui.Row(Ui.Btn("Open data folder", () => { Directory.CreateDirectory(Paths.Data); Open(Paths.Data); }),
                       Ui.Btn("Open manager log", () => { string p = Path.Combine(Paths.Data, "manager.log"); if (File.Exists(p)) Process.Start(new ProcessStartInfo(p) { UseShellExecute = true }); })))));

            var reset = new DockPanel();
            var rb = Ui.Btn("Reset modding...", ResetModding);
            rb.VerticalAlignment = VerticalAlignment.Center;
            rb.Margin = new Thickness(32, 0, 0, 0);
            DockPanel.SetDock(rb, Dock.Right);
            reset.Children.Add(rb);
            reset.Children.Add(Ui.Stack(Ui.H("Reset modding"), Ui.T("Start clean: the loader, the mods and the combined data leave the game folder, so the game is back to its original files. They're kept in the ModManager folder, not deleted.", 14, Theme.Muted)));
            s.Children.Add(Ui.Card(reset));
            return s;
        }

        void ResetModding()
        {
            if (Launcher.GameRunning()) throw new Exception("Close the game first.");
            if (!Dialogs.Confirm(this, "Reset modding", "Take every mod out of the game?", "The mod loader, the mods folder and the combined data move into the ModManager folder (reset <date>), so the game is back to its original files. Nothing is deleted.", "Reset modding", true)) return;
            string dst = Path.Combine(Paths.Data, "reset " + DateTime.Now.ToString("yyyy-MM-dd HHmmss"));
            Directory.CreateDirectory(dst);
            if (File.Exists(Paths.LoaderDll)) File.Move(Paths.LoaderDll, Path.Combine(dst, "winmm.dll"));
            if (Directory.Exists(Paths.Mods)) Directory.Move(Paths.Mods, Path.Combine(dst, "mods"));
            string dm = Path.Combine(Paths.Game, "DataMod");
            if (Directory.Exists(dm)) Directory.Move(dm, Path.Combine(dst, "DataMod"));
            Log.Write("reset modding: moved the loader, mods and DataMod to " + dst);
            Notice.Show("The game is vanilla again. Everything that was taken out is in the ModManager folder.");
            Show("settings");
        }

        // ---- Build a mod (for modders) ----
        UIElement BuildPage()
        {
            var s = Ui.Stack(Ui.PageHead("The forge", "Build a mod",
                "Turn a mod's sheets and pictures into a finished mod, ready to test and to share."));
            var steps = Ui.Stack(Ui.H("How it works"), Ui.Gap(10));
            string[] st = {
                "Make a project: a folder with mod.ini (name, version, author) and sheets like classes.csv, items.csv or text.csv, plus pictures.",
                "Edit the sheets in Excel or any spreadsheet program, and the pictures in any paint program.",
                "Build: problems are listed in plain words; a good build lands in your mods folder, ready to test.",
                "Share: the mod's folder (zipped) is the whole mod. Anyone can drop it on their Mod Manager.",
            };
            for (int i = 0; i < st.Length; i++)
            {
                var row = new DockPanel { Margin = new Thickness(0, 0, 0, 12) };
                var n = new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(13), BorderBrush = Theme.Gold, BorderThickness = new Thickness(1), Margin = new Thickness(0, 0, 14, 0), Child = Center(Ui.T((i + 1).ToString(), 13, Theme.Gold, true)), VerticalAlignment = VerticalAlignment.Top };
                DockPanel.SetDock(n, Dock.Left);
                row.Children.Add(n);
                row.Children.Add(Ui.T(st[i], 14, Theme.Muted));
                steps.Children.Add(row);
            }
            s.Children.Add(Ui.Card(steps));
            s.Children.Add(ToolsCard());
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Building from here"), Ui.T("For now, mods are built with the modding tools' Build mod.bat (get them above). Building straight from this page is planned.", 14, Theme.Muted))));
            return s;
        }

        bool toolsBusy;

        // The modding tools: get them from the mod list (checked like any mod), update, open.
        UIElement ToolsCard()
        {
            var card = Ui.Stack(Ui.H("Modding tools"),
                Ui.T("Build mod, Export sprites, the class maker and the other tools, with the Python they run on, so nothing else needs installing. Only for making mods: playing never needs them.", 14, Theme.Muted),
                Ui.Gap(12));
            if (ModList.Current == null && !listTried) LoadListSoon();
            var offer = ModList.Current == null ? null : ModList.Current.tools;
            string have = ModList.ToolsVersion, folder = ModList.ToolsFolder;
            var btns = new List<UIElement>();
            if (have != "")
            {
                card.Children.Add(Ui.T("Version " + have + " is in " + folder + ".", 14, Theme.Text));
                card.Children.Add(Ui.Gap(10));
                btns.Add(Ui.Btn("Open the folder", () => Process.Start("explorer.exe", "\"" + folder + "\""), true));
            }
            if (toolsBusy)
                card.Children.Add(Ui.T("Downloading the modding tools...", 14, Theme.Gold));
            else if (offer == null || string.IsNullOrEmpty(offer.url))
            {
                if (have == "")
                    card.Children.Add(Ui.T(listLoading ? "Looking at the mod list..." : "The mod list doesn't offer the modding tools yet.", 14, Theme.Muted));
            }
            else if (have == "" || ModList.CompareVersions(have, offer.version) < 0)
            {
                string what = (have == "" ? "Get the modding tools" : "Update to " + offer.version) + (offer.size > 0 ? " (" + Ui.Size(offer.size) + ")" : "");
                btns.Add(Ui.Btn(what, () => GetTools(offer, folder), have == "", false,
                    "Downloads them into " + folder + ", checked against the mod list. Your own mod folders are left alone."));
            }
            if (!toolsBusy)
                btns.Add(Ui.Btn("Change folder...", () =>
                {
                    using (var d = new System.Windows.Forms.FolderBrowserDialog { Description = "Where should the modding tools go?", SelectedPath = Directory.Exists(folder) ? folder : Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments) })
                        if (d.ShowDialog() == System.Windows.Forms.DialogResult.OK)
                        {
                            string f = d.SelectedPath;
                            if (!File.Exists(Path.Combine(f, "Build mod.bat")) && Directory.Exists(f) && Directory.EnumerateFileSystemEntries(f).Any())
                                f = Path.Combine(f, "DMFD Modding");  // don't spill into a busy folder
                            Ini.Set(Paths.SettingsFile, "tools", "folder", f);
                            if (!File.Exists(Path.Combine(f, "Build mod.bat"))) Ini.Set(Paths.SettingsFile, "tools", "version", "");
                            Refresh();
                        }
                }, false, false, "Pick where the modding tools go (or where you already put them)"));
            card.Children.Add(Ui.Row(btns.ToArray()));
            return Ui.Card(card);
        }

        void GetTools(ListVersion v, string folder)
        {
            if (toolsBusy) return;
            toolsBusy = true;
            Refresh();
            new Thread(() =>
            {
                string problem = null;
                try { ModList.SetUpTools(v, folder); }
                catch (Exception e) { problem = e.Message; Log.Write("modding tools: " + e); }
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    toolsBusy = false;
                    if (problem != null) Notice.Show(problem, true);
                    else Notice.Show("The modding tools " + v.version + " are in " + folder + ". Start with docs\\modding.md there.");
                    if (current == "build") Refresh();
                }));
            }) { IsBackground = true }.Start();
        }

        // ---- About ----
        UIElement AboutPage()
        {
            var s = Ui.Stack(Ui.PageHead("About", "DMFD Mod Manager", null));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Made for the Dragon Marked for Death community"),
                Ui.T("An unofficial, fan-made mod kit: the mod loader, this Mod Manager and the modding tools.", 14, Theme.Muted),
                Ui.KeyValues("Version", "Mod Manager " + Version,
                             "Made by", "Scootermouse: idea, design, art direction and testing",
                             "Programming", "Claude (Anthropic's AI), working with Scootermouse",
                             "Testing", "Scootermouse and friends from the DMFD community",
                             "Thanks", "Layout and several features inspired by UE Mod Manager (UE MM) by KintaroModding."))));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Dragon Marked for Death"),
                Ui.T("Dragon Marked for Death © INTI CREATES CO., LTD. All rights reserved. The game, its name, characters, art, music and logo belong to Inti Creates.", 14, Theme.Muted),
                Ui.Gap(8),
                Ui.T("This kit is not made, endorsed or supported by Inti Creates. It needs your own copy of the game and changes nothing in the game's own files; the game's art shown here is read from your own Steam library. Steam is a trademark of Valve Corporation.", 14, Theme.Muted))));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Licence"),
                Ui.T("The mod kit is free software under the GNU General Public License, version 3 or later: you may share it and change it, and versions you share must stay free under the same licence, with their source. It comes with no warranty. The full text is in LICENSE, next to the kit's source.", 14, Theme.Muted),
                Ui.Gap(8),
                Ui.T("Mods made for the kit belong to the people who make them, who choose their own licence (a mod's mod.ini can name it, and it shows on the Mods page). The small mod API that code mods build on is MIT-licensed, so any licence works for them.", 14, Theme.Muted))));
            s.Children.Add(Ui.Card(Ui.Stack(Ui.H("Open-source parts"),
                Ui.T("Dear ImGui © Omar Cornut (MIT licence): the in-game menu.", 14, Theme.Muted),
                Ui.T("MinHook © Tsuda Kageyu (BSD 2-clause licence): changing the game's code while it runs.", 14, Theme.Muted))));
            return s;
        }

        static void Open(string dir)
        {
            if (Directory.Exists(dir)) Process.Start("explorer.exe", "\"" + dir + "\"");
        }

        // ---- testing: draw a page into a picture without showing the window ----
        public void Snapshot(string pageId, string png, int w, int h)
        {
            Show(pageId);
            var el = (FrameworkElement)Content;
            el.Measure(new Size(w, h));
            el.Arrange(new Rect(0, 0, w, h));
            el.UpdateLayout();
            FitIntro();
            el.UpdateLayout();
            var bmp = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
            var bg = new DrawingVisual();
            using (var dc = bg.RenderOpen()) dc.DrawRectangle(Theme.Bg, null, new Rect(0, 0, w, h));
            bmp.Render(bg);
            bmp.Render(el);
            var enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(bmp));
            using (var f = File.Create(png)) enc.Save(f);
        }
    }

    static class Program
    {
        public const string ShowEvent = @"Local\DMFDModManager.Show";  // the loader's menu signals it too

        [STAThread]
        static int Main(string[] args)
        {
            string game = null, saves = null, snapPage = null, snapPng = null, cmd = null, cmdArg = null;
            int snapW = 1440, snapH = 920;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--game" && i + 1 < args.Length) game = args[++i];
                else if (args[i] == "--saves" && i + 1 < args.Length) saves = args[++i];
                else if (args[i] == "--no-steam-art") Paths.NoSteamArt = true;
                else if (args[i] == "--hero" && i + 1 < args.Length) Paths.HeroOverride = args[++i];
                else if (args[i] == "--snapshot" && i + 2 < args.Length) { snapPage = args[++i]; snapPng = args[++i]; }
                else if (args[i] == "--size" && i + 1 < args.Length) { var wh = args[++i].Split('x'); snapW = int.Parse(wh[0]); snapH = int.Parse(wh[1]); }
                else if (args[i] == "--combine") cmd = "combine";
                else if (args[i] == "--combine-to" && i + 1 < args.Length) { cmd = "combine"; cmdArg = args[++i]; }
                else if (args[i] == "--backup") cmd = "backup";
                else if (args[i] == "--room-fix") cmd = "roomfix";
                else if (args[i] == "--install-kit") cmd = "installkit";
                else if (args[i] == "--mod-list" && i + 1 < args.Length) ModList.Override = args[++i];
                else if (args[i] == "--list-install" && i + 1 < args.Length) { cmd = "listinstall"; cmdArg = args[++i]; }
                else if (args[i] == "--install" && i + 1 < args.Length) { cmd = "install"; cmdArg = args[++i]; }
                else if (args[i] == "--tools-setup" && i + 1 < args.Length) { cmd = "tools"; cmdArg = args[++i]; }
            }
            Paths.Find(game, saves);
            Log.Write("manager " + MainWindow.Version + " started; game " + (Paths.GameOk ? Paths.Game : "not found"));
            // testing without the window: --combine, --install <zip or folder> (results in manager.log)
            if (cmd == "combine")
            {
                List<string> ov;
                bool ok = Combiner.Run(line => Log.Write("combine: " + line), out ov, cmdArg);
                Log.Write("test combine: " + (ok ? "ok" : "failed") + ", up to date now: " + Combiner.UpToDate(ModStore.List()));
                return ok ? 0 : 1;
            }
            if (cmd == "listinstall")  // testing: install a mod (by id, latest) from the mod list, headless
            {
                try
                {
                    ModList.Load();
                    var m = ModList.Find(cmdArg);
                    if (m == null) throw new Exception("not on the list: " + cmdArg + (ModList.Problem != null ? " (" + ModList.Problem + ")" : ""));
                    Log.Write("test list install: installed " + ModList.Install(m, m.Latest));
                    return 0;
                }
                catch (Exception e) { Log.Write("test list install: " + e.Message); return 1; }
            }
            if (cmd == "tools")  // testing: the Build a mod page's "Get the modding tools", headless
            {
                try
                {
                    ModList.Load();
                    if (ModList.Current == null || ModList.Current.tools == null) throw new Exception("the list has no tools" + (ModList.Problem != null ? " (" + ModList.Problem + ")" : ""));
                    ModList.SetUpTools(ModList.Current.tools, cmdArg);
                    Log.Write("test tools: version now " + ModList.ToolsVersion + " in " + ModList.ToolsFolder);
                    return 0;
                }
                catch (Exception e) { Log.Write("test tools: " + e.Message); return 1; }
            }
            if (cmd == "installkit")  // testing: the Home card's "Install into the game folder", headless
            {
                try { Log.Write("test kit: " + Kit.Install()); return 0; }
                catch (Exception e) { Log.Write("test kit: " + e.Message); return 1; }
            }
            if (cmd == "roomfix")  // testing: what the Home card's room profile would do, done headless
            {
                try
                {
                    bool host;
                    var mods = ModStore.List();
                    var r = RoomCheck.Compare(mods, RoomCheck.Read(out host));
                    Log.Write("test room: install [" + string.Join("; ", r.Install) + "] versions [" + string.Join("; ", r.Versions) +
                              "] disable [" + string.Join("; ", r.Disable) + "] switch on [" + string.Join("; ", r.SwitchOn) + "]");
                    if (r.CanFix) RoomCheck.MakeProfile("Room test", mods, r);
                    Log.Write("test room: now " + string.Join(", ", ModStore.List().Select(m => m.Folder + (m.Enabled ? "+" : "-"))));
                    return 0;
                }
                catch (Exception e) { Log.Write("test room: " + e); return 1; }
            }
            if (cmd == "backup")
            {
                try { Log.Write("test backup: " + SaveBackups.Make("test") + "; " + SaveBackups.List().Count + " backup(s) kept"); return 0; }
                catch (Exception e) { Log.Write("test backup: " + e.Message); return 1; }
            }
            if (cmd == "install")
            {
                try { Log.Write("test install: installed " + ModStore.Install(cmdArg)); return 0; }
                catch (Exception e) { Log.Write("test install: " + e.Message); return 1; }
            }
            // One manager at a time: a second start (from the desktop, or the in-game menu's "Open the
            // Mod Manager") brings the open one to the front instead. The loader signals ShowEvent itself.
            Mutex single = null;
            EventWaitHandle showEvent = null;
            if (snapPage == null)
            {
                bool first;
                single = new Mutex(true, @"Local\DMFDModManager.Instance", out first);
                if (!first)
                {
                    try { EventWaitHandle.OpenExisting(ShowEvent).Set(); } catch (Exception) { }
                    Log.Write("already open: brought the open manager to the front");
                    return 0;
                }
                showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEvent);
            }
            var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
            try { app.Resources.MergedDictionaries.Add((ResourceDictionary)System.Windows.Markup.XamlReader.Parse(Theme.ScrollBars)); }
            catch (Exception e) { Log.Write("scrollbar style: " + e.Message); }
            if (snapPage != null && Paths.GameOk) ModList.Load();  // snapshots: the list in place before drawing
            var win = new MainWindow();
            if (snapPage != null)
            {
                try { win.Snapshot(snapPage, snapPng, snapW, snapH); }
                catch (Exception e) { Log.Write("snapshot failed: " + e); return 1; }
                return 0;
            }
            new Thread(() =>
            {
                while (showEvent.WaitOne())
                    win.Dispatcher.BeginInvoke(new Action(win.BringToFront));
            }) { IsBackground = true }.Start();
            int code = app.Run(win);
            GC.KeepAlive(single);
            return code;
        }
    }
}
