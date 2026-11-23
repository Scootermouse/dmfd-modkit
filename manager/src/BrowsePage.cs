// SPDX-License-Identifier: GPL-3.0-or-later
// The Browse page: the approved mod list (Browse.cs) as cards to search, install and update.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DmfdManager
{
    partial class MainWindow
    {
        string browseFilter = "", browseTag = "all";
        readonly HashSet<string> browseOpen = new HashSet<string>();  // cards showing their details
        bool listLoading, listTried;
        readonly HashSet<string> installing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // Fetch the list in the background, then redraw the page (pages that use it).
        void LoadListSoon()
        {
            if (listLoading) return;
            listLoading = true;
            listTried = true;
            new Thread(() =>
            {
                try { ModList.Load(); } catch (Exception e) { Log.Write("mod list: " + e.Message); }
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    listLoading = false;
                    if (current == "browse" || current == "home" || current == "mods" || current == "build") Refresh();
                }));
            }) { IsBackground = true }.Start();
        }

        // Download, check and install mods from the list one after another, then redraw.
        void InstallFromList(List<KeyValuePair<ListMod, ListVersion>> items)
        {
            items = items.Where(i => !installing.Contains(i.Key.id)).ToList();
            if (items.Count == 0) return;
            foreach (var i in items) installing.Add(i.Key.id);
            Notice.Show("Downloading " + string.Join(", ", items.Select(i => i.Key.name + " " + i.Value.version)) + "...");
            Refresh();
            new Thread(() =>
            {
                var done = new List<string>();
                string problem = null;
                bool data = false;
                foreach (var i in items)
                {
                    try
                    {
                        ModList.Install(i.Key, i.Value);
                        done.Add(i.Key.name + " " + i.Value.version);
                        data |= !(i.Key.type ?? "").Equals("code", StringComparison.OrdinalIgnoreCase);
                    }
                    catch (Exception e) { problem = e.Message; Log.Write("mod list install: " + e.Message); }
                }
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    foreach (var i in items) installing.Remove(i.Key.id);
                    if (problem != null) Notice.Show(problem, true);
                    else Notice.Show("Installed " + string.Join(", ", done) + "." + (Launcher.GameRunning() ? " Restart the game to use it." : ""));
                    if (data && !Launcher.GameRunning()) CombineSoon();
                    Refresh();
                }));
            }) { IsBackground = true }.Start();
        }

        UIElement BrowsePage()
        {
            var s = Ui.Stack(Ui.PageHead("Get more", "Browse",
                "Mods from the approved mod list. Each download is checked against the list before it's installed."));
            if (!Paths.GameOk) { s.Children.Add(NoGame()); return s; }
            if (ModList.Current == null && !listTried) LoadListSoon();

            // where the list comes from, and refreshing it
            string status = listLoading ? "Getting the mod list..."
                : ModList.Current == null ? (ModList.Problem ?? "No mod list yet.")
                : (ModList.Offline ? "Offline: showing the list saved " : "Mod list updated ") + ModList.LoadedAt.ToString("d MMM, HH:mm") +
                  " · " + ModList.Current.mods.Count + " mod(s)" + (ModList.Offline ? " (" + ModList.Problem + ")" : "");
            var bar = new DockPanel { Margin = new Thickness(0, 0, 0, 14) };
            var refresh = Ui.Btn("⟳ Refresh", LoadListSoon, false, false, "Get the mod list again");
            DockPanel.SetDock(refresh, Dock.Right);
            bar.Children.Add(refresh);
            var st = Ui.T(status, 13.5, ModList.Offline ? Theme.Gold : Theme.Muted);
            st.VerticalAlignment = VerticalAlignment.Center;
            st.TextWrapping = TextWrapping.Wrap;
            bar.Children.Add(st);
            s.Children.Add(bar);

            if (ModList.Current == null)
            {
                if (!listLoading)
                    s.Children.Add(Ui.Card(Ui.Stack(Ui.H("No mod list", 20),
                        Ui.T("The approved mod list isn't available yet. It comes with the mod kit's releases; for now mods are installed from their .zip files (Home, or drop one on this window).", 14, Theme.Muted))));
                return s;
            }

            var mods = ModStore.List();
            var list = ModList.Current.mods;
            var box = Ui.Input(browseFilter, "Search");
            var boxHost = Ui.WithHint(box, "Search: name, author, tag or description");
            boxHost.Margin = new Thickness(0, 0, 0, 10);
            s.Children.Add(boxHost);
            var tags = new[] { new[] { "all", "All (" + list.Count + ")" } }
                .Concat(list.SelectMany(m => m.tags).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(t => t)
                    .Select(t => new[] { t, char.ToUpper(t[0]) + t.Substring(1) + " (" + list.Count(m => m.tags.Contains(t, StringComparer.OrdinalIgnoreCase)) + ")" }))
                .Concat(new[] { new[] { "installed", "Installed (" + list.Count(m => Installed(mods, m) != null) + ")" } }).ToArray();
            var chips = Ui.Tabs(tags, browseTag, key => { browseTag = key; Show("browse"); }, true);

            var cards = new WrapPanel { Margin = new Thickness(18, 18, 0, 6) };
            Action fill = () =>
            {
                cards.Children.Clear();
                string f = browseFilter.Trim().ToLowerInvariant();
                foreach (var m in list)
                {
                    string hay = (m.name + " " + m.author + " " + m.summary + " " + m.description + " " + string.Join(" ", m.tags)).ToLowerInvariant();
                    if (f != "" && !hay.Contains(f)) continue;
                    if (browseTag == "installed" ? Installed(mods, m) == null
                        : browseTag != "all" && !m.tags.Contains(browseTag, StringComparer.OrdinalIgnoreCase)) continue;
                    cards.Children.Add(ModCard(mods, m));
                }
                if (cards.Children.Count == 0)
                    cards.Children.Add(new Border { Padding = new Thickness(30), Child = Ui.T("No mod matches.", 14, Theme.Muted) });
            };
            fill();
            box.TextChanged += (o, e) => { browseFilter = box.Text; fill(); };
            s.Children.Add(Ui.TabbedCard(chips, cards));
            return s;
        }

        static ModInfo Installed(List<ModInfo> mods, ListMod m)
        {
            return mods.FirstOrDefault(x => string.Equals(x.Folder, m.id, StringComparison.OrdinalIgnoreCase));
        }

        UIElement ModCard(List<ModInfo> mods, ListMod m)
        {
            var have = Installed(mods, m);
            var v = m.Latest;
            var body = new StackPanel();
            // picture (pixel art: kept sharp)
            if (m.images.Count > 0)
            {
                var img = new Image { Height = 150, Stretch = Stretch.Uniform, Margin = new Thickness(0, 0, 0, 12) };
                RenderOptions.SetBitmapScalingMode(img, BitmapScalingMode.NearestNeighbor);
                var frame = new Border { Child = img, Background = Theme.B("#120b0d"), CornerRadius = new CornerRadius(5), Margin = new Thickness(0, 0, 0, 12), Height = 150 };
                img.Margin = new Thickness(0);
                body.Children.Add(frame);
                string link = m.images[0];
                Action<string> show = file =>
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.UriSource = new Uri(file);
                        bi.EndInit();
                        img.Source = bi;
                    }
                    catch (Exception) { }
                };
                string local = ModList.Resolve(link);
                if (local != null && !local.StartsWith("http", StringComparison.OrdinalIgnoreCase) && File.Exists(local)) show(local);
                else
                    new Thread(() =>
                    {
                        string file = ModList.Picture(link);  // downloaded once, then kept
                        if (file != null) Dispatcher.BeginInvoke(new Action(() => show(file)));
                    }) { IsBackground = true }.Start();
            }
            var title = Ui.Row(Ui.T(m.name ?? m.id, 18, Theme.Text, true));
            body.Children.Add(title);
            body.Children.Add(Ui.T((m.author != null ? "by " + m.author + " · " : "") + "version " + v.version +
                                   (v.size > 0 ? " · " + Ui.Size(v.size) : "") + (string.IsNullOrEmpty(m.license) ? "" : " · " + m.license), 12.5, Theme.Muted));
            if (!string.IsNullOrEmpty(m.summary))
            {
                var sum = Ui.T(m.summary, 13.5, Theme.Text);
                sum.Margin = new Thickness(0, 8, 0, 0);
                sum.TextWrapping = TextWrapping.Wrap;
                body.Children.Add(sum);
            }
            var tagRow = Ui.Row();
            tagRow.Margin = new Thickness(0, 10, 0, 0);
            foreach (string t in m.tags) tagRow.Children.Add(Ui.Tag(t.ToUpperInvariant(), Theme.Muted, Theme.Line));
            if ((m.online ?? "") == "any")
            {
                var any = Ui.Tag("ANY ROOM", Theme.Green);
                any.ToolTip = "Only changes your own game: you can play online in any room with it, modded or not.";
                tagRow.Children.Add(any);
            }
            if (m.HasCode)
            {
                var code = Ui.Tag("PROGRAM", Theme.Gold);
                code.ToolTip = "Contains a program (a DLL) that runs with the game. Only mods on the approved list are offered here.";
                tagRow.Children.Add(code);
            }
            body.Children.Add(tagRow);

            // what you can do with it
            var actions = Ui.Row();
            actions.Margin = new Thickness(0, 12, 0, 0);
            string state = ModList.State(have, m);
            bool busy = installing.Contains(m.id);
            Func<StackPanel, StackPanel> mid = c => { c.VerticalAlignment = VerticalAlignment.Center; c.Margin = new Thickness(0, 0, 12, 0); return c; };
            if (busy) actions.Children.Add(mid(Check("Installing...", true, true)));
            else if (state == "missing")
                actions.Children.Add(Ui.Btn("Install", () => InstallFromList(new List<KeyValuePair<ListMod, ListVersion>> { new KeyValuePair<ListMod, ListVersion>(m, v) }), true));
            else if (state == "update")
                actions.Children.Add(Ui.Btn("Update to " + v.version, () => InstallFromList(new List<KeyValuePair<ListMod, ListVersion>> { new KeyValuePair<ListMod, ListVersion>(m, v) }), true,
                                            false, "You have " + have.Version + ". The update keeps the mod switched on or off as it is."));
            else actions.Children.Add(mid(Check(state == "newer" ? "Installed (" + have.Version + ", newer than the list)" : "Installed" + (have.Enabled ? "" : " (switched off)"), true)));
            bool open = browseOpen.Contains(m.id);
            actions.Children.Add(Ui.Btn(open ? "Less" : "More", () => { if (!browseOpen.Remove(m.id)) browseOpen.Add(m.id); Refresh(); }));
            if (!string.IsNullOrEmpty(m.homepage))
                actions.Children.Add(Ui.Btn("Discuss", () => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(ModList.Resolve(m.homepage)) { UseShellExecute = true }), false, false, "Its page, to ask questions or report problems"));
            body.Children.Add(actions);

            if (open)
            {
                var more = new StackPanel { Margin = new Thickness(0, 14, 0, 0) };
                if (!string.IsNullOrEmpty(m.description))
                {
                    var d = Ui.T(m.description, 13.5, Theme.Muted);
                    d.TextWrapping = TextWrapping.Wrap;
                    more.Children.Add(d);
                    more.Children.Add(Ui.Gap(10));
                }
                more.Children.Add(Ui.Label("VERSIONS"));
                foreach (var ver in m.versions.Take(6))
                {
                    var line = new DockPanel { Margin = new Thickness(0, 6, 0, 0) };
                    bool mine = have != null && have.Version == ver.version;
                    if (!mine && !busy)
                    {
                        var vv = ver;
                        var b = Ui.Btn(ver == v ? "Install" : "Install this one", () => InstallFromList(new List<KeyValuePair<ListMod, ListVersion>> { new KeyValuePair<ListMod, ListVersion>(m, vv) }), false, true,
                                       ver == v ? null : "An older version, e.g. to match an online room that uses it");
                        DockPanel.SetDock(b, Dock.Right);
                        line.Children.Add(b);
                    }
                    var txt = Ui.T(ver.version + (string.IsNullOrEmpty(ver.date) ? "" : " · " + ver.date) + (mine ? " · installed" : "") +
                                   (string.IsNullOrEmpty(ver.changes) ? "" : "\n" + ver.changes), 13, mine ? Theme.Green : Theme.Text);
                    txt.TextWrapping = TextWrapping.Wrap;
                    line.Children.Add(txt);
                    more.Children.Add(line);
                }
                body.Children.Add(more);
            }
            return new Border
            {
                Width = 380, Margin = new Thickness(0, 0, 18, 18), Padding = new Thickness(20, 18, 20, 18), Child = body,
                Background = Theme.Panel, BorderBrush = state == "update" ? Theme.Gold : Theme.Line, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8),
            };
        }

        // Home: installed mods with a newer version on the list.
        UIElement UpdatesCard(List<ModInfo> mods)
        {
            if (ModList.Current == null) return null;
            var ups = ModList.Current.mods.Select(m => new { m, have = Installed(mods, m) })
                .Where(x => x.have != null && ModList.State(x.have, x.m) == "update").ToList();
            if (ups.Count == 0) return null;
            return Ui.Card(Ui.Stack(Ui.H("Updates available", 20),
                Ui.T(string.Join(", ", ups.Select(x => x.m.name + " " + x.have.Version + " → " + x.m.Latest.version)), 14, Theme.Muted),
                Ui.Gap(12), Ui.Row(
                    Ui.Btn("Update all", () => InstallFromList(ups.Select(x => new KeyValuePair<ListMod, ListVersion>(x.m, x.m.Latest)).ToList()), true),
                    Ui.Btn("Browse", () => Show("browse")))), null, Theme.Blue);
        }
    }
}
