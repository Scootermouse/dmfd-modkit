// The manager's look (from manager/mockup): colors, fonts and the few controls it's built
// from, made in code (no XAML compiler needed to build it).
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;

namespace DmfdManager
{
    static class Theme
    {
        public static Color C(string hex) { return (Color)ColorConverter.ConvertFromString(hex); }
        public static SolidColorBrush B(string hex) { var b = new SolidColorBrush(C(hex)); b.Freeze(); return b; }

        public static readonly SolidColorBrush Bg = B("#120b0d"), Side = B("#0d0809"), Panel = B("#EB1d1315"),
            PanelSolid = B("#1d1315"), Line = B("#3b282b"), LineSoft = B("#2a1c1f"), Text = B("#f3e9dc"),
            Muted = B("#b3a196"), Faint = B("#7d6d66"), Red = B("#d42a35"), RedDark = B("#9e1822"),
            Gold = B("#e6b84e"), GoldSoft = B("#1Fe6b84e"), Green = B("#7cc77f"), Blue = B("#7fb2e8"),
            MenuBlue = B("#1f3fd6"), MenuBlueHi = B("#4f8dff"), Violet = B("#6a2bb8"), Warn = B("#ffb24d"),
            Error = B("#ff7a7f"), Hover = B("#0DFFFFFF");

        public static readonly FontFamily Serif = new FontFamily("Georgia"), Sans = new FontFamily("Segoe UI"),
            Mono = new FontFamily("Consolas");

        // Thin dark scrollbars instead of Windows' light ones (loaded as XAML at start-up).
        public const string ScrollBars = @"
<ResourceDictionary xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'
                    xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
  <Style TargetType='ToolTip'>
    <Setter Property='Foreground' Value='#f3e9dc'/>
    <Setter Property='FontFamily' Value='Segoe UI'/>
    <Setter Property='FontSize' Value='13'/>
    <Setter Property='MaxWidth' Value='420'/>
    <Setter Property='Template'>
      <Setter.Value>
        <ControlTemplate TargetType='ToolTip'>
          <Border Background='#1d1315' BorderBrush='#5a3d41' BorderThickness='1' CornerRadius='5' Padding='12,8,12,8'>
            <ContentPresenter>
              <ContentPresenter.Resources>
                <Style TargetType='TextBlock'><Setter Property='TextWrapping' Value='Wrap'/></Style>
              </ContentPresenter.Resources>
            </ContentPresenter>
          </Border>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>
  <Style TargetType='ScrollBar'>
    <Setter Property='Width' Value='10'/>
    <Setter Property='MinWidth' Value='10'/>
    <Setter Property='Background' Value='Transparent'/>
    <Setter Property='Template'>
      <Setter.Value>
        <ControlTemplate TargetType='ScrollBar'>
          <Track x:Name='PART_Track' IsDirectionReversed='True'>
            <Track.Thumb>
              <Thumb>
                <Thumb.Template>
                  <ControlTemplate TargetType='Thumb'>
                    <Border Background='#5a3d41' CornerRadius='4' Margin='2,2,2,2'/>
                  </ControlTemplate>
                </Thumb.Template>
              </Thumb>
            </Track.Thumb>
          </Track>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
    <Style.Triggers>
      <Trigger Property='Orientation' Value='Horizontal'>
        <Setter Property='Width' Value='Auto'/>
        <Setter Property='Height' Value='10'/>
        <Setter Property='Template'>
          <Setter.Value>
            <ControlTemplate TargetType='ScrollBar'>
              <Track x:Name='PART_Track'>
                <Track.Thumb>
                  <Thumb>
                    <Thumb.Template>
                      <ControlTemplate TargetType='Thumb'>
                        <Border Background='#5a3d41' CornerRadius='4' Margin='2,2,2,2'/>
                      </ControlTemplate>
                    </Thumb.Template>
                  </Thumb>
                </Track.Thumb>
              </Track>
            </ControlTemplate>
          </Setter.Value>
        </Setter>
      </Trigger>
    </Style.Triggers>
  </Style>
</ResourceDictionary>";
    }

    static class Ui
    {
        public static TextBlock T(string text, double size = 14, Brush brush = null, bool bold = false, FontFamily font = null)
        {
            return new TextBlock
            {
                Text = text, FontSize = size, Foreground = brush ?? Theme.Text, TextWrapping = TextWrapping.Wrap,
                FontWeight = bold ? FontWeights.SemiBold : FontWeights.Normal, FontFamily = font ?? Theme.Sans,
            };
        }

        public static TextBlock Label(string text, Brush brush = null)
        {
            var t = T(text.ToUpperInvariant(), 11, brush ?? Theme.Faint, true);
            t.Margin = new Thickness(0, 0, 0, 2);
            return t;
        }

        public static TextBlock Kicker(string text) { var t = T(text.ToUpperInvariant(), 11.5, Theme.Red, true); t.Margin = new Thickness(0, 0, 0, 6); return t; }

        public static TextBlock H(string text, double size = 23) { return T(text, size, Theme.Text, true, Theme.Serif); }

        // A panel. With an amber (Theme.Gold) or red (Theme.Error) border it's an alert: a thicker
        // border, a tinted background and a caution sign, so it isn't read past.
        public static Border Card(UIElement child, Thickness? pad = null, Brush border = null)
        {
            bool alert = border == Theme.Gold || border == Theme.Error;
            if (alert)
            {
                var sign = T("⚠", 30, border, true);
                sign.Margin = new Thickness(0, -4, 18, 0);
                sign.VerticalAlignment = VerticalAlignment.Top;
                var dock = new DockPanel();
                DockPanel.SetDock(sign, Dock.Left);
                dock.Children.Add(sign);
                dock.Children.Add(child);
                child = dock;
            }
            return new Border
            {
                Background = alert ? Theme.B(border == Theme.Error ? "#F22a1214" : "#F22a2010") : Theme.Panel,
                BorderBrush = border ?? Theme.Line, BorderThickness = new Thickness(alert ? 2 : 1),
                CornerRadius = new CornerRadius(8), Padding = pad ?? new Thickness(26, 24, 26, 24),
                Margin = new Thickness(0, 0, 0, 20), Child = child,
            };
        }

        public static StackPanel Stack(params UIElement[] kids)
        {
            var s = new StackPanel();
            foreach (var k in kids) if (k != null) s.Children.Add(k);
            return s;
        }

        public static WrapPanel Row(params UIElement[] kids)
        {
            var s = new WrapPanel { Orientation = Orientation.Horizontal };
            foreach (var k in kids)
                if (k != null)
                {
                    if (k is FrameworkElement fe && fe.Margin == default(Thickness)) fe.Margin = new Thickness(0, 0, 10, 10);
                    s.Children.Add(k);
                }
            return s;
        }

        public static FrameworkElement Gap(double h) { return new Border { Height = h }; }

        // What each button does, shown when the mouse rests on it.
        static readonly System.Collections.Generic.Dictionary<string, string> Tips = new System.Collections.Generic.Dictionary<string, string>
        {
            { "Install from .zip...", "Choose a mod's .zip file; it's checked, then put in the mods folder." },
            { "Install from folder...", "Choose a mod's folder (the one with mod.ini); it's copied into the mods folder." },
            { "Open game folder", "Opens the game's folder in Explorer." },
            { "Open mods folder", "Opens the mods folder in Explorer." },
            { "Prepare mods now", "Combines the switched-on data mods so the game uses them. Takes about half a minute." },
            { "⟳ Refresh", "Reads the folders again (for changes made outside the manager)." },
            { "Copy", "Copies the log shown below, so you can paste it into a message." },
            { "Open log file", "Opens this log in your text editor." },
            { "Report a problem...", "Puts every log and your mod list into one .zip and shows it in Explorer, ready to send to whoever is helping you." },
            { "Back up now", "Copies your save and the extra classes' save into a backup right now." },
            { "Open save folder", "Opens the folder with your save in Explorer." },
            { "Open backups", "Opens the folder with your save backups in Explorer." },
            { "Restore", "Puts this backup back as your save. Your current save is backed up first." },
            { "Locate game folder...", "Choose the Dragon Marked for Death folder (the one with game.exe) yourself." },
            { "Search again", "Looks for the game through Steam again." },
            { "Choose save folder...", "Choose the folder with SystemData.bin yourself." },
            { "Open data folder", "Opens the manager's own folder (settings, backups, reports)." },
            { "Open manager log", "Opens the manager's own log in your text editor." },
            { "Reset modding...", "Takes the loader, the mods and the combined data out of the game folder (kept, not deleted). Asks first." },
            { "Folder", "Opens this mod's folder in Explorer." },
            { "↑", "Load earlier: mods lower in the list win when two change the same thing." },
            { "↓", "Load later: mods lower in the list win when two change the same thing." },
            { "Save current as...", "Saves which mods are on, and their order, as a profile you can switch back to." },
        };

        // A clickable button made from a border (no template needed).
        public static Border Btn(string text, Action click, bool primary = false, bool small = false, string tip = null)
        {
            var t = T(text, small ? 13 : 14, Theme.Text, primary);
            t.TextWrapping = TextWrapping.NoWrap;
            var b = new Border
            {
                Child = t, Cursor = Cursors.Hand, CornerRadius = new CornerRadius(5),
                Padding = small ? new Thickness(10, 5, 10, 5) : new Thickness(15, 9, 15, 9),
                BorderThickness = new Thickness(1), VerticalAlignment = VerticalAlignment.Top,
                Background = primary ? (Brush)new LinearGradientBrush(Theme.C("#d42a35"), Theme.C("#9e1822"), 90) : Theme.B("#F21d1315"),
                BorderBrush = primary ? Theme.B("#e8505a") : Theme.Line,
                ToolTip = tip ?? (Tips.ContainsKey(text) ? Tips[text] : null),
            };
            Brush normal = b.Background;
            b.MouseEnter += (s, e) => { if (!primary) b.Background = Theme.B("#F22d1f22"); b.BorderBrush = primary ? Theme.B("#ff7a82") : Theme.B("#63454a"); };
            b.MouseLeave += (s, e) => { b.Background = normal; b.BorderBrush = primary ? Theme.B("#e8505a") : Theme.Line; };
            b.MouseLeftButtonUp += (s, e) => { e.Handled = true; Guard(click); };
            return b;
        }

        // Runs a click handler; problems are shown in plain words, never a crash.
        public static void Guard(Action a)
        {
            try { a(); }
            catch (Exception ex) { Notice.Show(ex.Message, true); Log.Write("problem: " + ex); }
        }

        // A profile-style pill: click the name to pick it, the × to remove it.
        public static Border Chip(string text, bool active, Action pick, Action remove, string tip, string removeTip)
        {
            var name = T(text, 14, active ? Brushes.White : Theme.Text, active);
            name.TextWrapping = TextWrapping.NoWrap;
            var nameBox = new Border { Child = name, Padding = new Thickness(15, 9, 12, 9), Background = Brushes.Transparent, Cursor = Cursors.Hand, ToolTip = tip };
            nameBox.MouseLeftButtonUp += (s, e) => { e.Handled = true; Guard(pick); };
            var x = T("×", 16, Theme.Muted);
            x.VerticalAlignment = VerticalAlignment.Center;
            var xBox = new Border { Child = x, Padding = new Thickness(10, 6, 12, 6), VerticalAlignment = VerticalAlignment.Stretch, Background = Brushes.Transparent, Cursor = Cursors.Hand, ToolTip = removeTip,
                                    BorderBrush = active ? Theme.B("#664f8dff") : Theme.Line, BorderThickness = new Thickness(1, 0, 0, 0) };
            xBox.MouseEnter += (s, e) => x.Foreground = Theme.Error;
            xBox.MouseLeave += (s, e) => x.Foreground = Theme.Muted;
            xBox.MouseLeftButtonUp += (s, e) => { e.Handled = true; Guard(remove); };
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            sp.Children.Add(nameBox);
            sp.Children.Add(xBox);
            var chip = new Border
            {
                Child = sp, CornerRadius = new CornerRadius(5), BorderThickness = new Thickness(1),
                Background = active ? Theme.B("#731f3fd6") : Theme.B("#F21d1315"), BorderBrush = active ? Theme.MenuBlueHi : Theme.Line,
                Margin = new Thickness(0, 0, 10, 10), VerticalAlignment = VerticalAlignment.Top,
            };
            if (!active)
            {
                chip.MouseEnter += (s, e) => chip.BorderBrush = Theme.B("#63454a");
                chip.MouseLeave += (s, e) => chip.BorderBrush = Theme.Line;
            }
            return chip;
        }

        // A row of tabs. joined: they sit on top of the panel that follows (see TabbedCard), and
        // the selected tab fades from the menu blue into that panel's brown.
        public static StackPanel Tabs(string[][] items, string selected, Action<string> pick, bool joined)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            foreach (var it in items)
            {
                string key = it[0];
                bool on = key == selected;
                var t = T(it[1], 13.5, on ? Brushes.White : Theme.Muted, on);
                t.TextWrapping = TextWrapping.NoWrap;
                var tab = new Border
                {
                    Child = t, Padding = new Thickness(16, 7, 16, joined ? 8 : 7), Margin = new Thickness(0, 0, 4, joined ? 0 : 10),
                    Cursor = on ? Cursors.Arrow : Cursors.Hand, VerticalAlignment = VerticalAlignment.Bottom,
                    CornerRadius = joined ? new CornerRadius(6, 6, 0, 0) : new CornerRadius(5),
                    BorderThickness = joined ? new Thickness(1, 1, 1, 0) : new Thickness(1),
                };
                if (on)
                {
                    var bg = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
                    bg.GradientStops.Add(new GradientStop(Theme.C("#B31f3fd6"), 0));
                    bg.GradientStops.Add(new GradientStop(joined ? Theme.C("#1d1315") : Theme.C("#731f3fd6"), 1));
                    tab.Background = bg;
                    var edge = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
                    edge.GradientStops.Add(new GradientStop(Theme.C("#4f8dff"), 0));
                    edge.GradientStops.Add(new GradientStop(joined ? Theme.C("#3b282b") : Theme.C("#4f8dff"), 1));
                    tab.BorderBrush = edge;
                }
                else
                {
                    tab.Background = Theme.B("#F21d1315");
                    tab.BorderBrush = Theme.Line;
                    tab.MouseEnter += (s, e) => { tab.Background = Theme.B("#F22d1f22"); t.Foreground = Theme.Text; };
                    tab.MouseLeave += (s, e) => { tab.Background = Theme.B("#F21d1315"); t.Foreground = Theme.Muted; };
                    tab.MouseLeftButtonUp += (s, e) => { e.Handled = true; Guard(() => pick(key)); };
                }
                row.Children.Add(tab);
            }
            return row;
        }

        // Tabs on top of a card: the selected tab covers the card's top edge, so they join.
        public static Grid TabbedCard(StackPanel tabs, UIElement content)
        {
            var g = new Grid { Margin = new Thickness(0, 0, 0, 20) };
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            g.RowDefinitions.Add(new RowDefinition());
            var card = new Border
            {
                Background = Theme.Panel, BorderBrush = Theme.Line, BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(0, 8, 8, 8), Child = content,
            };
            Grid.SetRow(card, 1);
            g.Children.Add(card);
            tabs.Margin = new Thickness(0, 0, 0, -1);  // over the card's top edge
            Panel.SetZIndex(tabs, 1);
            g.Children.Add(tabs);
            return g;
        }

        public static Border Tag(string text, Brush fg, Brush border = null)
        {
            var t = T(text, 11, fg, true);
            return new Border
            {
                Child = t, BorderBrush = border ?? fg, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(3),
                Padding = new Thickness(8, 2, 8, 2), Margin = new Thickness(0, 0, 6, 0), VerticalAlignment = VerticalAlignment.Center,
            };
        }

        public static Border Badge(string text)
        {
            return new Border
            {
                Child = T(text, 12, Theme.Text, true), Background = Theme.B("#12FFFFFF"), CornerRadius = new CornerRadius(3),
                Padding = new Thickness(9, 3, 9, 3), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center,
            };
        }

        // The red-on switch from the mockup.
        public static Border Toggle(bool on, Action<bool> changed, string onText = "On", string offText = "Off", bool locked = false)
        {
            var knob = new Ellipse { Width = 16, Height = 16, Fill = on ? Brushes.White : Theme.B("#cbb9ad"), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(on ? 23 : 3, 0, 0, 0) };
            var track = new Border { Width = 42, Height = 22, CornerRadius = new CornerRadius(11), Background = on ? Theme.Red : Theme.B("#3a2a2d"), Child = knob };
            var label = T(on ? onText : offText, 14, on ? Theme.Text : Theme.Muted);
            label.Margin = new Thickness(10, 0, 0, 0);
            label.VerticalAlignment = VerticalAlignment.Center;
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            sp.Children.Add(track);
            sp.Children.Add(label);
            var b = new Border { Child = sp, Background = Brushes.Transparent, Cursor = locked ? Cursors.Arrow : Cursors.Hand, VerticalAlignment = VerticalAlignment.Center };
            if (!locked)
                b.MouseLeftButtonUp += (s, e) =>
                {
                    e.Handled = true;
                    on = !on;
                    knob.Margin = new Thickness(on ? 23 : 3, 0, 0, 0);
                    knob.Fill = on ? Brushes.White : Theme.B("#cbb9ad");
                    track.Background = on ? Theme.Red : Theme.B("#3a2a2d");
                    label.Text = on ? onText : offText;
                    label.Foreground = on ? Theme.Text : Theme.Muted;
                    Guard(() => changed(on));
                };
            return b;
        }

        public static TextBox Input(string text, string hint, double width = double.NaN)
        {
            var t = new TextBox
            {
                Text = text, Background = Theme.B("#E60d0809"), Foreground = Theme.Text, BorderBrush = Theme.Line,
                BorderThickness = new Thickness(1), Padding = new Thickness(12, 8, 12, 8), FontSize = 14, FontFamily = Theme.Sans,
                CaretBrush = Theme.Text, Width = width, VerticalContentAlignment = VerticalAlignment.Center, ToolTip = hint,
            };
            t.GotFocus += (s, e) => t.BorderBrush = Theme.MenuBlueHi;
            t.LostFocus += (s, e) => t.BorderBrush = Theme.Line;
            return t;
        }

        // A wide "download into" icon: an arrow into a tray.
        public static FrameworkElement DownloadIcon()
        {
            var p = new Path
            {
                Data = Geometry.Parse("M 24,3 V 25 M 13,15 L 24,26 L 35,15 M 5,25 V 35 H 43 V 25"),
                Stroke = Theme.Gold, StrokeThickness = 3.2, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
                StrokeLineJoin = PenLineJoin.Round, Width = 48, Height = 38, HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 0, 0, 8),
            };
            return p;
        }

        // An input with gray hint text while it's empty.
        public static Grid WithHint(TextBox box, string hint)
        {
            var g = new Grid();
            var h = T(hint, 14, Theme.Faint);
            h.IsHitTestVisible = false;
            h.Margin = new Thickness(14, 0, 0, 0);
            h.VerticalAlignment = VerticalAlignment.Center;
            h.Visibility = box.Text == "" ? Visibility.Visible : Visibility.Collapsed;
            box.TextChanged += (s, e) => h.Visibility = box.Text == "" ? Visibility.Visible : Visibility.Collapsed;
            if (!double.IsNaN(box.Width)) g.Width = box.Width;
            g.Children.Add(box);
            g.Children.Add(h);
            return g;
        }

        // Key / value pairs in two columns.
        public static Grid KeyValues(params string[] kv)
        {
            var g = new Grid { Margin = new Thickness(0, 6, 0, 14) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(170) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            for (int i = 0; i + 1 < kv.Length; i += 2)
            {
                g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                var k = Label(kv[i]);
                k.Margin = new Thickness(0, 4, 12, 6);
                bool path = kv[i + 1].Contains(":\\");
                var v = T(kv[i + 1], path ? 13 : 14, Theme.Text, false, path ? Theme.Mono : null);
                v.Margin = new Thickness(0, 2, 0, 6);
                Grid.SetRow(k, i / 2);
                Grid.SetRow(v, i / 2);
                Grid.SetColumn(v, 1);
                g.Children.Add(k);
                g.Children.Add(v);
            }
            return g;
        }

        public static Rectangle Rule()
        {
            var b = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
            b.GradientStops.Add(new GradientStop(Theme.C("#d42a35"), 0));
            b.GradientStops.Add(new GradientStop(Theme.C("#6a2bb8"), 0.6));
            b.GradientStops.Add(new GradientStop(Theme.C("#006a2bb8"), 1));
            return new Rectangle { Width = 320, Height = 3, Fill = b, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 14, 0, 18) };
        }

        public static TextBlock LastIntro;  // the shown page's intro (kept beside the header art's busy part)

        public static StackPanel PageHead(string eyebrow, string title, string intro)
        {
            LastIntro = null;
            var s = new StackPanel();
            var e = T(eyebrow.ToUpperInvariant(), 11.5, Theme.Red, true);
            e.Effect = new DropShadowEffect { BlurRadius = 6, ShadowDepth = 0, Opacity = 1, Color = Colors.Black };
            s.Children.Add(e);
            var h = T(title, 44, Theme.Text, true, Theme.Serif);
            h.Effect = new DropShadowEffect { BlurRadius = 12, ShadowDepth = 2, Opacity = 0.7, Color = Colors.Black };
            h.Margin = new Thickness(0, 4, 0, 0);
            s.Children.Add(h);
            s.Children.Add(Rule());
            if (intro != null)
            {
                // readable over the header art: brighter than other muted text, with a dark glow
                var i = T(intro, 15, Theme.B("#ddd0c4"));
                i.Effect = new DropShadowEffect { BlurRadius = 8, ShadowDepth = 0, Opacity = 1, Color = Colors.Black };
                i.MaxWidth = 660;
                LastIntro = i;
                i.HorizontalAlignment = HorizontalAlignment.Left;
                i.Margin = new Thickness(0, 0, 0, 26);
                s.Children.Add(i);
            }
            return s;
        }

        public static string Size(long bytes)
        {
            if (bytes >= 1 << 20) return (bytes / 1048576.0).ToString("0.0") + " MB";
            if (bytes >= 1 << 10) return (bytes / 1024.0).ToString("0") + " KB";
            return bytes + " bytes";
        }
    }


    // Dialog boxes in the manager's own style (instead of Windows' gray message boxes).
    static class Dialogs
    {
        [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        // A dark title bar like UE MM's (Windows 10 2004+ and 11; older Windows keeps its own).
        public static void DarkTitle(Window w)
        {
            try
            {
                IntPtr h = new System.Windows.Interop.WindowInteropHelper(w).Handle;
                int on = 1;
                if (DwmSetWindowAttribute(h, 20, ref on, 4) != 0) DwmSetWindowAttribute(h, 19, ref on, 4);  // 19: Windows 10 before 2004
                int caption = 0x0d | 0x08 << 8 | 0x09 << 16;  // Windows 11: the sidebar's color (COLORREF 0x00BBGGRR)
                DwmSetWindowAttribute(h, 35, ref caption, 4);
            }
            catch { }
        }

        static Window Make(Window owner, string title)
        {
            var w = new Window
            {
                Title = title, Owner = owner, Width = 480, SizeToContent = SizeToContent.Height, ResizeMode = ResizeMode.NoResize,
                WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = Theme.PanelSolid, FontFamily = Theme.Sans,
                ShowInTaskbar = false, Icon = owner != null ? owner.Icon : null,
            };
            w.SourceInitialized += (o, e) => DarkTitle(w);
            return w;
        }

        // "Are you sure?" with the action named on the button. True when confirmed.
        public static bool Confirm(Window owner, string title, string heading, string text, string okText, bool danger = false)
        {
            var w = Make(owner, title);
            bool result = false;
            var ok = Ui.Btn(okText, () => { result = true; w.Close(); }, !danger);
            if (danger) { ok.BorderBrush = Theme.Error; ((TextBlock)ok.Child).Foreground = Theme.Error; }
            var buttons = Ui.Row(ok, Ui.Btn("Cancel", () => w.Close()));
            buttons.Margin = new Thickness(0, 18, 0, 0);
            w.Content = new Border { Padding = new Thickness(26, 22, 26, 12), Child = Ui.Stack(Ui.H(heading, 20), Ui.Gap(8), Ui.T(text, 14, Theme.Muted), buttons) };
            w.KeyDown += (o, e) => { if (e.Key == Key.Escape) w.Close(); };
            w.ShowDialog();
            return result;
        }

        // A one-line text box. Null when cancelled.
        public static string Ask(Window owner, string title, string prompt, string okText)
        {
            var w = Make(owner, title);
            var box = Ui.Input("", prompt);
            string result = null;
            Action ok = () => { result = box.Text; w.Close(); };
            box.KeyDown += (o, e) => { if (e.Key == Key.Enter) ok(); if (e.Key == Key.Escape) w.Close(); };
            var buttons = Ui.Row(Ui.Btn(okText, ok, true), Ui.Btn("Cancel", () => w.Close()));
            buttons.Margin = new Thickness(0, 16, 0, 0);
            w.Content = new Border { Padding = new Thickness(26, 22, 26, 12), Child = Ui.Stack(Ui.H(title, 20), Ui.Gap(8), Ui.T(prompt, 14, Theme.Muted), Ui.Gap(10), box, buttons) };
            w.Loaded += (o, e) => box.Focus();
            w.ShowDialog();
            return result;
        }
    }

    // A short message at the bottom (like the mockup's toast).
    static class Notice
    {
        public static Border Host;
        static System.Windows.Threading.DispatcherTimer timer;

        public static void Show(string text, bool problem = false)
        {
            if (Host == null) return;
            var t = Ui.T(text, 14, Theme.Text);
            t.MaxWidth = 460;
            Host.Child = t;
            Host.BorderBrush = problem ? Theme.Error : Theme.Gold;
            Host.Visibility = Visibility.Visible;
            if (timer == null)
            {
                timer = new System.Windows.Threading.DispatcherTimer();
                timer.Tick += (s, e) => { Host.Visibility = Visibility.Collapsed; timer.Stop(); };
            }
            timer.Stop();
            timer.Interval = TimeSpan.FromSeconds(problem ? 8 : 4);
            timer.Start();
        }
    }
}
