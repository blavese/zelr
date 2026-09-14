using System.Diagnostics;
using System.Windows;
using System.Windows.Media;
using System.Windows.Navigation;

namespace ZelrLauncher;

public partial class MainWindow : Window
{
    private string? _qemu;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += (_, _) => Refresh();
    }

    private static SolidColorBrush Brush(string key) =>
        (SolidColorBrush)Application.Current.Resources[key];

    private void Refresh()
    {
        _qemu = Emulator.FindQemu();

        if (_qemu is not null)
        {
            StatusDot.Fill = Brush("Accent");
            StatusTitle.Text = "Ready";
            StatusDetail.Text = "The emulator is installed. Press Start and a black window will open with zelr running inside it.";
            StartButton.IsEnabled = true;
            SetupButton.Visibility = Visibility.Collapsed;
        }
        else
        {
            StatusDot.Fill = Brush("Warn");
            StatusTitle.Text = "One thing is missing";
            StatusDetail.Text =
                "zelr needs QEMU, a free program that pretends to be a computer so the operating system has something to boot on. " +
                "Set up the emulator installs it from Microsoft's own package manager. It is about 150 MB and takes a minute.";
            StartButton.IsEnabled = false;
            SetupButton.Visibility = Visibility.Visible;
        }
    }

    private void Start_Click(object sender, RoutedEventArgs e)
    {
        if (_qemu is null) return;
        try
        {
            StartButton.IsEnabled = false;
            StatusTitle.Text = "Starting";
            StatusDetail.Text = "A separate black window is opening. That window is the machine.";

            var kernel = Emulator.ExtractKernel();
            var proc = Emulator.Start(_qemu, kernel);

            proc.EnableRaisingEvents = true;
            proc.Exited += (_, _) => Dispatcher.Invoke(() =>
            {
                StatusTitle.Text = "Ready";
                StatusDetail.Text = "zelr closed. Press Start to boot it again.";
                StartButton.IsEnabled = true;
            });
        }
        catch (Exception ex)
        {
            StatusDot.Fill = Brush("Warn");
            StatusTitle.Text = "It did not start";
            StatusDetail.Text = ex.Message;
            StartButton.IsEnabled = true;
        }
    }

    private void Setup_Click(object sender, RoutedEventArgs e)
    {
        var answer = MessageBox.Show(
            "This installs QEMU, a free open source emulator, using Windows' own package manager (winget).\n\n" +
            "A console window will appear so you can see exactly what it does. Nothing else on your computer is changed.\n\n" +
            "Install it now?",
            "Set up the emulator",
            MessageBoxButton.YesNo, MessageBoxImage.Question);

        if (answer != MessageBoxResult.Yes) return;

        try
        {
            SetupButton.IsEnabled = false;
            StatusTitle.Text = "Installing";
            StatusDetail.Text = "Follow the console window. This page will update when it finishes.";

            var proc = Emulator.InstallQemu();
            proc.EnableRaisingEvents = true;
            proc.Exited += (_, _) => Dispatcher.Invoke(() =>
            {
                SetupButton.IsEnabled = true;
                Refresh();
                if (_qemu is null)
                {
                    StatusTitle.Text = "Still not found";
                    StatusDetail.Text =
                        "The install may not have completed, or Windows has not picked it up yet. " +
                        "Try Check again, or install QEMU yourself from qemu.org and come back.";
                }
            });
        }
        catch (Exception ex)
        {
            SetupButton.IsEnabled = true;
            StatusTitle.Text = "Could not run the installer";
            StatusDetail.Text =
                ex.Message + "  You can install QEMU yourself from qemu.org, then press Check again.";
        }
    }

    private void Recheck_Click(object sender, RoutedEventArgs e) => Refresh();

    private void Link_Click(object sender, RequestNavigateEventArgs e)
    {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
        e.Handled = true;
    }
}
