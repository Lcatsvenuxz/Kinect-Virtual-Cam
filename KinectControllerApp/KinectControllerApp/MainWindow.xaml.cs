using System;
using System.Collections.Generic;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Kinect;

namespace KinectControllerApp
{
    public partial class MainWindow : Window
    {
        // ====================================================================
        //  PROTOCOLO IPC (memoria compartida "KinectSharedMem") - VERSION 2
        //  Debe coincidir byte a byte con KinectCamFilter/dllmain.cpp
        // ====================================================================
        private const string SharedMemName = "KinectSharedMem";
        private const uint IpcMagic = 0x54434B49;
        private const int IpcVersion = 2;
        private const int HeaderSize = 64;
        private const int IpcMaxWidth = 1280;
        private const int IpcMaxHeight = 960;
        private const int TotalSize = HeaderSize + (IpcMaxWidth * IpcMaxHeight * 4);

        private const int OffMagic = 0;
        private const int OffVersion = 4;
        private const int OffWidth = 8;
        private const int OffHeight = 12;
        private const int OffStride = 16;
        private const int OffFormat = 20;
        private const int OffFps = 24;
        private const int OffActive = 28;
        private const int OffSequence = 32;
        private const int OffTimestamp = 40;
        private const int OffReqWidth = 48;
        private const int OffReqHeight = 52;

        private enum SensorConnState { Searching, Connected, Disconnected, Error, IrUnsupported }

        private KinectSensor kinectSensor;
        private MemoryMappedFile sharedMemory;
        private MemoryMappedViewAccessor memoryAccessor;
        private unsafe byte* shmPtr = null;
        private bool isStreaming = false;
        private bool isFrozen = false;
        private bool suppressEmitterEvent = false;
        private bool isLoadingPrefs = false;

        private byte[] pixelData;
        private WriteableBitmap bitmap;
        private bool bitmapIsIr;

        private int currentLang = 0; // 0 = Español, 1 = English
        private SensorConnState sensorState = SensorConnState.Searching;
        private string sensorStateExtra = "";
        private int lastClientReqW = 0;
        private int lastClientReqH = 0;

        private Color customBgColor = Color.FromRgb(0x24, 0x24, 0x28);
        private Color customAccentColor = Color.FromRgb(0x3A, 0x86, 0xFF);
        private Color activeStartBtnColor = Color.FromRgb(0x28, 0xA7, 0x45);

        private DispatcherTimer clientTimer;

        // Tilt diferido (debounce de 300 ms en hilo de fondo)
        private DispatcherTimer tiltTimer;
        private int tiltTarget = 0;
        private int tiltBusy = 0;

        // Bandeja del sistema
        private System.Windows.Forms.NotifyIcon notifyIcon;
        private System.Windows.Forms.ToolStripMenuItem trayOpenItem;
        private System.Windows.Forms.ToolStripMenuItem trayExitItem;

        private readonly string prefsFilePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "KinectControllerApp", "preferences.ini");

        public MainWindow()
        {
            InitializeComponent();
            this.Loaded += MainWindow_Loaded;
            this.Closed += MainWindow_Closed;
            this.StateChanged += MainWindow_StateChanged;
        }

        // ------------------------------------------------------------------
        //  Inicio
        // ------------------------------------------------------------------
        private unsafe void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                sharedMemory = MemoryMappedFile.CreateOrOpen(SharedMemName, TotalSize);
                memoryAccessor = sharedMemory.CreateViewAccessor(0, TotalSize);
                byte* p = null;
                memoryAccessor.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
                shmPtr = p + memoryAccessor.PointerOffset;

                *(uint*)(shmPtr + OffMagic) = IpcMagic;
                *(int*)(shmPtr + OffVersion) = IpcVersion;
                *(int*)(shmPtr + OffActive) = 0;
                *(int*)(shmPtr + OffReqWidth) = 0;
                *(int*)(shmPtr + OffReqHeight) = 0;
            }
            catch
            {
                MessageBox.Show("Error IPC: no se pudo crear la memoria compartida de 4.9 MB.\n" +
                                "Si una versión anterior del filtro sigue cargada en otra aplicación, ciérrala y reinicia este programa.");
            }

            InitTrayIcon();

            tiltTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) };
            tiltTimer.Tick += TiltTimer_Tick;
            sliderTilt.ValueChanged += SliderTilt_ValueChanged;

            LoadPreferences();

            // Publica de inmediato la resolucion seleccionada en el encabezado IPC
            // para que cualquier cliente DirectShow negocie el modo 4:3 exacto del sensor.
            GetSelectedModeDimensions(out int initW, out int initH, out int initFps);
            PublishHeader(initW, initH, initFps);

            ApplyTheme(cmbTheme.SelectedIndex);
            ApplyLanguage(cmbLanguage.SelectedIndex);

            KinectSensor.KinectSensors.StatusChanged += KinectSensors_StatusChanged;
            InitSensor();

            clientTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            clientTimer.Tick += ClientTimer_Tick;
            clientTimer.Start();
        }

        private void GetSelectedModeDimensions(out int width, out int height, out int fps)
        {
            if (rbColorRgbHD != null && rbColorRgbHD.IsChecked == true)
            {
                width = 1280;
                height = 960;
                fps = 12;
            }
            else
            {
                width = 640;
                height = 480;
                fps = 30;
            }
        }

        private void InitSensor()
        {
            if (kinectSensor != null) return;

            foreach (KinectSensor s in KinectSensor.KinectSensors)
            {
                if (s.Status == KinectStatus.Connected) { kinectSensor = s; break; }
            }

            if (kinectSensor == null)
            {
                sensorState = SensorConnState.Searching;
                sensorStateExtra = "";
                RefreshStatusTexts();
                return;
            }

            kinectSensor.ColorFrameReady += KinectSensor_ColorFrameReady;
            sensorState = SensorConnState.Connected;
            sensorStateExtra = "";
            RefreshStatusTexts();
            ApplyColorFormat();
        }

        private void KinectSensors_StatusChanged(object sender, StatusChangedEventArgs e)
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (e.Status == KinectStatus.Connected)
                {
                    InitSensor();
                }
                else if (kinectSensor != null && e.Sensor == kinectSensor)
                {
                    kinectSensor.ColorFrameReady -= KinectSensor_ColorFrameReady;
                    kinectSensor = null;
                    isStreaming = false;
                    isFrozen = false;
                    SetActive(false);
                    UpdateResolutionControlsState();
                    RefreshActionButtonVisual();
                    RefreshFreezeButtonVisual();
                    sensorState = SensorConnState.Disconnected;
                    sensorStateExtra = e.Status.ToString();
                    RefreshStatusTexts();
                }
            }));
        }

        // ------------------------------------------------------------------
        //  Seleccion de formato NATIVO (la eleccion del usuario es ABSOLUTA)
        // ------------------------------------------------------------------
        private void ColorFormat_Checked(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || isLoadingPrefs || isStreaming) return;

            if (rbColorOff != null && rbColorOff.IsChecked != true)
            {
                GetSelectedModeDimensions(out int selW, out int selH, out int selFps);
                PublishHeader(selW, selH, selFps);
            }

            ApplyColorFormat();
        }

        private void ApplyColorFormat()
        {
            if (kinectSensor == null) return;

            try
            {
                if (rbColorOff.IsChecked == true)
                {
                    kinectSensor.ColorStream.Disable();
                    cameraFeed.Source = null;
                    bitmap = null;
                    pixelData = null;
                    SetActive(false);
                    return;
                }

                ColorImageFormat format = ColorImageFormat.RgbResolution640x480Fps30;
                if (rbColorRgbHD.IsChecked == true) format = ColorImageFormat.RgbResolution1280x960Fps12;
                else if (rbColorIr.IsChecked == true) format = ColorImageFormat.InfraredResolution640x480Fps30;

                if (kinectSensor.IsRunning) kinectSensor.ColorStream.Disable();

                int width = (format == ColorImageFormat.RgbResolution1280x960Fps12) ? 1280 : 640;
                int height = (format == ColorImageFormat.RgbResolution1280x960Fps12) ? 960 : 480;
                int fps = (format == ColorImageFormat.RgbResolution1280x960Fps12) ? 12 : 30;
                bool isIr = (format == ColorImageFormat.InfraredResolution640x480Fps30);

                pixelData = new byte[width * height * (isIr ? 2 : 4)];
                bitmapIsIr = isIr;
                bitmap = new WriteableBitmap(width, height, 96.0, 96.0, isIr ? PixelFormats.Gray16 : PixelFormats.Bgr32, null);
                cameraFeed.Source = bitmap;

                kinectSensor.ColorStream.Enable(format);
                if (!kinectSensor.IsRunning) kinectSensor.Start();

                PublishHeader(width, height, fps);
            }
            catch (Exception ex)
            {
                sensorState = SensorConnState.Error;
                sensorStateExtra = ex.Message;
                RefreshStatusTexts();
            }
        }

        private unsafe void PublishHeader(int width, int height, int fps)
        {
            if (shmPtr == null) return;
            byte* b = shmPtr;
            long* seq = (long*)(b + OffSequence);

            Interlocked.Increment(ref *seq);
            *(int*)(b + OffWidth) = width;
            *(int*)(b + OffHeight) = height;
            *(int*)(b + OffStride) = width * 4;
            *(int*)(b + OffFormat) = 0;
            *(int*)(b + OffFps) = fps;
            *(int*)(b + OffActive) = isStreaming ? 1 : 0;
            Interlocked.Increment(ref *seq);
        }

        private void ChkForceOffIr_Changed(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || suppressEmitterEvent || kinectSensor == null) return;
            try
            {
                kinectSensor.ForceInfraredEmitterOff = (chkForceOffIr.IsChecked == true);
            }
            catch (Exception)
            {
                suppressEmitterEvent = true;
                chkForceOffIr.IsChecked = false;
                suppressEmitterEvent = false;
                sensorState = SensorConnState.IrUnsupported;
                RefreshStatusTexts();
            }
        }

        private void SliderTilt_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            tiltTarget = (int)e.NewValue;
            if (txtTiltAngle != null) txtTiltAngle.Text = $"{tiltTarget}°";

            if (tiltTimer != null) { tiltTimer.Stop(); tiltTimer.Start(); }
        }

        private void TiltTimer_Tick(object sender, EventArgs e)
        {
            tiltTimer.Stop();

            KinectSensor sensor = kinectSensor;
            if (sensor == null || !sensor.IsRunning) return;

            if (Interlocked.CompareExchange(ref tiltBusy, 1, 0) != 0) { tiltTimer.Start(); return; }

            int angle = tiltTarget;
            System.Threading.Tasks.Task.Run(() =>
            {
                try
                {
                    int clamped = Math.Max(sensor.MinElevationAngle, Math.Min(sensor.MaxElevationAngle, angle));
                    sensor.ElevationAngle = clamped;
                }
                catch (Exception) { }
                finally { Interlocked.Exchange(ref tiltBusy, 0); }
            });
        }

        // ------------------------------------------------------------------
        //  Inyeccion, Congelar Imagen y proteccion de resolucion
        // ------------------------------------------------------------------
        private void BtnToggleInjection_Click(object sender, RoutedEventArgs e)
        {
            if (kinectSensor == null) return;

            if (!isStreaming)
            {
                if (rbColorOff.IsChecked == true)
                    rbColorRgbSD.IsChecked = true;

                GetSelectedModeDimensions(out int selW, out int selH, out int selFps);
                isStreaming = true;
                isFrozen = false;
                PublishHeader(selW, selH, selFps);
                UpdateResolutionControlsState();
                RefreshActionButtonVisual();
                RefreshFreezeButtonVisual();
            }
            else
            {
                isStreaming = false;
                isFrozen = false;
                SetActive(false);
                UpdateResolutionControlsState();
                RefreshActionButtonVisual();
                RefreshFreezeButtonVisual();
            }
        }

        private void BtnFreezeFrame_Click(object sender, RoutedEventArgs e)
        {
            if (!isStreaming) return;
            isFrozen = !isFrozen;
            RefreshFreezeButtonVisual();
        }

        private void UpdateResolutionControlsState()
        {
            bool canChange = !isStreaming;
            rbColorRgbSD.IsEnabled = canChange;
            rbColorRgbHD.IsEnabled = canChange;
            rbColorIr.IsEnabled = canChange;
            rbColorOff.IsEnabled = canChange;
            txtResolutionWarning.Visibility = isStreaming ? Visibility.Visible : Visibility.Collapsed;
            if (btnFreezeFrame != null) btnFreezeFrame.IsEnabled = isStreaming;
        }

        private void RefreshActionButtonVisual()
        {
            if (isStreaming)
            {
                btnToggleInjection.Content = (currentLang == 1) ? "STOP KINECT WEBCAM" : "DETENER KINECT WEBCAM";
                btnToggleInjection.Background = Brushes.DarkRed;
            }
            else
            {
                btnToggleInjection.Content = (currentLang == 1) ? "START KINECT WEBCAM" : "INICIAR KINECT WEBCAM";
                btnToggleInjection.Background = new SolidColorBrush(activeStartBtnColor);
            }
        }

        private void RefreshFreezeButtonVisual()
        {
            if (btnFreezeFrame == null || txtFreezeWarning == null) return;
            bool en = (currentLang == 1);

            if (isStreaming && isFrozen)
            {
                btnFreezeFrame.Content = en ? "UNFREEZE IMAGE (Resume)" : "DESCONGELAR IMAGEN (Reanudar)";
                btnFreezeFrame.Background = new SolidColorBrush(Color.FromRgb(0xD9, 0x77, 0x06));
                txtFreezeWarning.Text = en ? "IMAGE FROZEN (Webcam paused)" : "IMAGEN CONGELADA (Webcam en pausa)";
                txtFreezeWarning.Visibility = Visibility.Visible;
            }
            else
            {
                btnFreezeFrame.Content = en ? "FREEZE IMAGE" : "CONGELAR IMAGEN";
                btnFreezeFrame.Background = new SolidColorBrush(Color.FromRgb(0x49, 0x50, 0x57));
                txtFreezeWarning.Visibility = Visibility.Collapsed;
            }
        }

        private unsafe void SetActive(bool active)
        {
            if (shmPtr == null) return;
            Volatile.Write(ref *(int*)(shmPtr + OffActive), active ? 1 : 0);
        }

        // ------------------------------------------------------------------
        //  Captura de frames:
        //  - cameraFeed muestra SIEMPRE la imagen original cruda del Kinect
        //    (sin Flip Horizontal ni Flip Vertical).
        //  - chkFlipH y chkFlipV se aplican EXCLUSIVAMENTE al escribir en IPC.
        // ------------------------------------------------------------------
        private void KinectSensor_ColorFrameReady(object sender, ColorImageFrameReadyEventArgs e)
        {
            using (ColorImageFrame colorFrame = e.OpenColorImageFrame())
            {
                if (colorFrame == null) return;

                int width = colorFrame.Width;
                int height = colorFrame.Height;
                bool isIr = (colorFrame.Format == ColorImageFormat.InfraredResolution640x480Fps30);
                int bytesPerPixel = isIr ? 2 : 4;
                int fps = (colorFrame.Format == ColorImageFormat.RgbResolution1280x960Fps12) ? 12 : 30;

                if (pixelData == null || pixelData.Length != colorFrame.PixelDataLength || bitmap == null ||
                    bitmap.PixelWidth != width || bitmap.PixelHeight != height || bitmapIsIr != isIr)
                {
                    pixelData = new byte[colorFrame.PixelDataLength];
                    bitmapIsIr = isIr;
                    bitmap = new WriteableBitmap(width, height, 96.0, 96.0, isIr ? PixelFormats.Gray16 : PixelFormats.Bgr32, null);
                    cameraFeed.Source = bitmap;
                }

                colorFrame.CopyPixelDataTo(pixelData);

                // Previsualizacion interna en WPF: SIEMPRE cruda del sensor, sin espejo ni inversion
                if (IsVisible)
                    bitmap.WritePixels(new Int32Rect(0, 0, width, height), pixelData, width * bytesPerPixel, 0);

                if (!isStreaming) return;

                PublishFrame(width, height, fps, isIr, chkFlipH.IsChecked == true, chkFlipV.IsChecked == true, colorFrame.Timestamp * 10000L);
            }
        }

        private unsafe void PublishFrame(int width, int height, int fps, bool isIr, bool flipH, bool flipV, long timestamp100ns)
        {
            if (shmPtr == null || width > IpcMaxWidth || height > IpcMaxHeight) return;

            // Si "Congelar Imagen" esta activo, suspendemos la actualizacion de pixeles
            // manteniendo OffActive = 1 y el ultimo fotograma valido intacto en memoria compartida.
            if (isFrozen) return;

            byte* b = shmPtr;
            long* seq = (long*)(b + OffSequence);

            Interlocked.Increment(ref *seq); // -> impar: escritura en curso

            *(int*)(b + OffWidth) = width;
            *(int*)(b + OffHeight) = height;
            *(int*)(b + OffStride) = width * 4;
            *(int*)(b + OffFormat) = 0;
            *(int*)(b + OffFps) = fps;
            *(long*)(b + OffTimestamp) = timestamp100ns;
            *(int*)(b + OffActive) = 1;

            uint* dst = (uint*)(b + HeaderSize);
            fixed (byte* srcBytes = pixelData)
            {
                if (!isIr)
                {
                    uint* src = (uint*)srcBytes;
                    if (!flipH && !flipV)
                    {
                        int count = width * height;
                        for (int i = 0; i < count; i++) dst[i] = src[i] | 0xFF000000u;
                    }
                    else
                    {
                        for (int y = 0; y < height; y++)
                        {
                            int srcY = flipV ? (height - 1 - y) : y;
                            uint* sRow = src + (long)srcY * width;
                            uint* dRow = dst + (long)y * width;
                            if (flipH) { for (int x = 0; x < width; x++) dRow[x] = sRow[width - 1 - x] | 0xFF000000u; }
                            else { for (int x = 0; x < width; x++) dRow[x] = sRow[x] | 0xFF000000u; }
                        }
                    }
                }
                else
                {
                    for (int y = 0; y < height; y++)
                    {
                        int srcY = flipV ? (height - 1 - y) : y;
                        byte* sRow = srcBytes + (long)srcY * width * 2;
                        uint* dRow = dst + (long)y * width;
                        for (int x = 0; x < width; x++)
                        {
                            int srcX = flipH ? (width - 1 - x) : x;
                            uint v = sRow[srcX * 2 + 1];
                            dRow[x] = (v * 0x010101u) | 0xFF000000u;
                        }
                    }
                }
            }

            Interlocked.Increment(ref *seq); // -> par: frame estable
        }

        // ------------------------------------------------------------------
        //  Actualizacion periodica de estado: muestra la resolucion conectada
        //  sin alterar los RadioButtons.
        // ------------------------------------------------------------------
        private unsafe void ClientTimer_Tick(object sender, EventArgs e)
        {
            if (shmPtr != null)
            {
                lastClientReqW = Volatile.Read(ref *(int*)(shmPtr + OffReqWidth));
                lastClientReqH = Volatile.Read(ref *(int*)(shmPtr + OffReqHeight));
            }
            else
            {
                lastClientReqW = 0;
                lastClientReqH = 0;
            }

            RefreshStatusTexts();
        }

        // ------------------------------------------------------------------
        //  Sistema de Color Limpio y Sobrio
        // ------------------------------------------------------------------
        private void CmbTheme_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded) return;
            ApplyTheme(cmbTheme.SelectedIndex);
        }

        private void ApplyTheme(int index)
        {
            if (pnlCustomColors != null)
                pnlCustomColors.Visibility = (index == 3) ? Visibility.Visible : Visibility.Collapsed;

            switch (index)
            {
                case 0: // Dark Purple (Original)
                    SetCleanColors(
                        ColorFromHex("#2D2D30"),
                        ColorFromHex("#4B367C"),
                        ColorFromHex("#FFFFFF"),
                        ColorFromHex("#28A745"));
                    break;

                case 1: // Midnight Slate
                    SetCleanColors(
                        ColorFromHex("#1B222C"),
                        ColorFromHex("#283342"),
                        ColorFromHex("#7A9BB8"),
                        ColorFromHex("#2B6CB0"));
                    break;

                case 2: // Obsidian
                    SetCleanColors(
                        ColorFromHex("#121212"),
                        ColorFromHex("#1E2221"),
                        ColorFromHex("#3D8B6E"),
                        ColorFromHex("#2E7D5B"));
                    break;

                default: // Personalizado
                    if (txtHexBg != null) txtHexBg.Text = ColorToHex(customBgColor);
                    if (txtHexAccent != null) txtHexAccent.Text = ColorToHex(customAccentColor);
                    Color panelTone = LightenColor(customBgColor, 18);
                    SetCleanColors(customBgColor, panelTone, customAccentColor, customAccentColor);
                    break;
            }

            if (IsLoaded) RefreshActionButtonVisual();
        }

        private void SetCleanColors(Color winBg, Color panelBg, Color accentBorder, Color actionBtn)
        {
            activeStartBtnColor = actionBtn;
            Resources["WindowBgBrush"] = new SolidColorBrush(winBg);
            Resources["PanelBgBrush"] = new SolidColorBrush(panelBg);
            Resources["AccentBrush"] = new SolidColorBrush(accentBorder);
            Resources["ActionBtnBrush"] = new SolidColorBrush(actionBtn);
        }

        private void CustomHex_LostFocus(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            Color c;
            if (TryParseHex(txtHexBg.Text, out c)) customBgColor = c;
            if (TryParseHex(txtHexAccent.Text, out c)) customAccentColor = c;
            if (cmbTheme.SelectedIndex == 3) ApplyTheme(3);
        }

        private void BtnPickBg_Click(object sender, RoutedEventArgs e)
        {
            customBgColor = PickColorDialog(customBgColor);
            if (cmbTheme.SelectedIndex == 3) ApplyTheme(3);
        }

        private void BtnPickAccent_Click(object sender, RoutedEventArgs e)
        {
            customAccentColor = PickColorDialog(customAccentColor);
            if (cmbTheme.SelectedIndex == 3) ApplyTheme(3);
        }

        private static Color PickColorDialog(Color current)
        {
            using (var dlg = new System.Windows.Forms.ColorDialog())
            {
                dlg.FullOpen = true;
                dlg.Color = System.Drawing.Color.FromArgb(current.R, current.G, current.B);
                if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK)
                    return Color.FromRgb(dlg.Color.R, dlg.Color.G, dlg.Color.B);
            }
            return current;
        }

        private static Color ColorFromHex(string hex)
        {
            Color c;
            return TryParseHex(hex, out c) ? c : Colors.Black;
        }

        private static bool TryParseHex(string hex, out Color color)
        {
            color = Colors.Black;
            if (string.IsNullOrWhiteSpace(hex)) return false;
            string s = hex.Trim();
            if (!s.StartsWith("#")) s = "#" + s;
            try
            {
                object obj = ColorConverter.ConvertFromString(s);
                if (obj is Color parsed) { color = parsed; return true; }
            }
            catch { }
            return false;
        }

        private static string ColorToHex(Color c) => $"#{c.R:X2}{c.G:X2}{c.B:X2}";

        private static Color LightenColor(Color c, int delta)
        {
            byte Clamp(int v) => (byte)(v < 0 ? 0 : (v > 255 ? 255 : v));
            return Color.FromRgb(Clamp(c.R + delta), Clamp(c.G + delta), Clamp(c.B + delta));
        }

        // ------------------------------------------------------------------
        //  Idioma Dinamico (Español / English)
        // ------------------------------------------------------------------
        private void CmbLanguage_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded) return;
            ApplyLanguage(cmbLanguage.SelectedIndex);
        }

        private void ApplyLanguage(int langIndex)
        {
            currentLang = (langIndex == 1) ? 1 : 0;
            bool en = (currentLang == 1);

            Title = en ? "Control Panel - Kinect Virtual Cam" : "Panel de Control - Kinect Virtual Cam";
            grpPreferences.Header = en ? "Language / Appearance" : "Idioma / Apariencia";
            lblLanguage.Text = en ? "Language:" : "Idioma:";
            lblTheme.Text = en ? "Theme:" : "Tema:";
            itemThemeCustom.Content = en ? "Custom" : "Personalizado";
            lblCustomBg.Text = en ? "Background:" : "Fondo:";
            lblCustomAccent.Text = en ? "Accent:" : "Acento:";
            btnPickBg.Content = en ? "Pick..." : "Color...";
            btnPickAccent.Content = en ? "Pick..." : "Color...";

            grpColorStream.Header = en ? "Color Stream" : "Color Stream";
            rbColorOff.Content = en ? "Off" : "Apagado (Off)";
            txtResolutionWarning.Text = en
                ? "Webcam active. Stop streaming to change resolution."
                : "Webcam activa. Detenga la transmisión para cambiar la resolución.";

            grpDepthStream.Header = en
                ? "Depth Stream (unused by virtual cam)"
                : "Depth Stream (no usado por la camara virtual)";
            lblDepthTreatment.Text = en ? "Depth Treatment" : "Tratamiento Depth";

            grpSensorSettings.Header = en ? "Sensor Settings" : "Ajustes del Sensor";
            lblTiltAngle.Text = en ? "Tilt Angle" : "Ángulo de Inclinación (Tilt)";
            lblImageOrientation.Text = en ? "Image Orientation" : "Orientación (solo Webcam)";
            chkFlipV.Content = en ? "Flip Vertical" : "Voltear Vertical";
            chkFlipH.Content = en ? "Flip Horizontal" : "Voltear Horizontal";
            lblIrEmitter.Text = en ? "Infrared Emitter" : "Emisor Infrarrojo";
            chkForceOffIr.Content = en ? "Force Off" : "Forzar Apagado";

            grpStatus.Header = en ? "Status" : "Estado";

            if (trayOpenItem != null) trayOpenItem.Text = en ? "Open Panel" : "Abrir panel";
            if (trayExitItem != null) trayExitItem.Text = en ? "Exit" : "Salir";

            RefreshActionButtonVisual();
            RefreshFreezeButtonVisual();
            RefreshStatusTexts();
        }

        private void RefreshStatusTexts()
        {
            if (txtSensorStatus == null || txtClientStatus == null) return;
            bool en = (currentLang == 1);

            switch (sensorState)
            {
                case SensorConnState.Connected:
                    txtSensorStatus.Text = en ? "Sensor: connected" : "Sensor: conectado";
                    break;
                case SensorConnState.Disconnected:
                    txtSensorStatus.Text = (en ? "Sensor: disconnected (" : "Sensor: desconectado (") + sensorStateExtra + ")";
                    break;
                case SensorConnState.Error:
                    txtSensorStatus.Text = (en ? "Sensor: error (" : "Sensor: error (") + sensorStateExtra + ")";
                    break;
                case SensorConnState.IrUnsupported:
                    txtSensorStatus.Text = en
                        ? "Sensor: this model cannot disable the IR emitter"
                        : "Sensor: este modelo no permite apagar el emisor IR";
                    break;
                default:
                    txtSensorStatus.Text = en ? "Sensor: not detected" : "Sensor: no detectado";
                    break;
            }

            if (lastClientReqW > 0 && lastClientReqH > 0)
            {
                txtClientStatus.Text = en
                    ? $"Client: connected at {lastClientReqW}x{lastClientReqH}"
                    : $"Cliente: conectado a {lastClientReqW}x{lastClientReqH}";
            }
            else
            {
                txtClientStatus.Text = en
                    ? "Client: none connected"
                    : "Cliente: ninguno conectado";
            }
        }

        // ------------------------------------------------------------------
        //  Persistencia de preferencias (.ini)
        // ------------------------------------------------------------------
        private void LoadPreferences()
        {
            if (!File.Exists(prefsFilePath)) return;

            isLoadingPrefs = true;
            try
            {
                var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (string line in File.ReadAllLines(prefsFilePath))
                {
                    int eq = line.IndexOf('=');
                    if (eq > 0) map[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
                }

                string v;
                int n;
                Color c;
                if (map.TryGetValue("Language", out v) && int.TryParse(v, out n) && (n == 0 || n == 1))
                    cmbLanguage.SelectedIndex = n;
                if (map.TryGetValue("CustomBg", out v) && TryParseHex(v, out c)) customBgColor = c;
                if (map.TryGetValue("CustomAccent", out v) && TryParseHex(v, out c)) customAccentColor = c;
                if (map.TryGetValue("Theme", out v) && int.TryParse(v, out n) && n >= 0 && n <= 3)
                    cmbTheme.SelectedIndex = n;

                if (map.TryGetValue("FlipH", out v)) chkFlipH.IsChecked = (v == "1");
                if (map.TryGetValue("FlipV", out v)) chkFlipV.IsChecked = (v == "1");

                if (map.TryGetValue("Mode", out v))
                {
                    if (v == "HD") rbColorRgbHD.IsChecked = true;
                    else if (v == "IR") rbColorIr.IsChecked = true;
                    else rbColorRgbSD.IsChecked = true;
                }
            }
            catch { }
            finally
            {
                isLoadingPrefs = false;
            }
        }

        private void SavePreferences()
        {
            try
            {
                string dir = Path.GetDirectoryName(prefsFilePath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

                string mode = "SD";
                if (rbColorRgbHD.IsChecked == true) mode = "HD";
                else if (rbColorIr.IsChecked == true) mode = "IR";

                string[] lines =
                {
                    "Language=" + cmbLanguage.SelectedIndex,
                    "Theme=" + cmbTheme.SelectedIndex,
                    "CustomBg=" + ColorToHex(customBgColor),
                    "CustomAccent=" + ColorToHex(customAccentColor),
                    "FlipH=" + (chkFlipH.IsChecked == true ? "1" : "0"),
                    "FlipV=" + (chkFlipV.IsChecked == true ? "1" : "0"),
                    "Mode=" + mode
                };
                File.WriteAllLines(prefsFilePath, lines);
            }
            catch { }
        }

        // ------------------------------------------------------------------
        //  Bandeja del sistema
        // ------------------------------------------------------------------
        private void InitTrayIcon()
        {
            notifyIcon = new System.Windows.Forms.NotifyIcon
            {
                Icon = System.Drawing.SystemIcons.Application,
                Text = "Kinect Virtual Cam",
                Visible = false
            };
            notifyIcon.DoubleClick += (s, a) => RestoreFromTray();

            var menu = new System.Windows.Forms.ContextMenuStrip();
            trayOpenItem = new System.Windows.Forms.ToolStripMenuItem("Abrir panel", null, (s, a) => RestoreFromTray());
            trayExitItem = new System.Windows.Forms.ToolStripMenuItem("Salir", null, (s, a) => { Close(); });
            menu.Items.Add(trayOpenItem);
            menu.Items.Add(trayExitItem);
            notifyIcon.ContextMenuStrip = menu;
        }

        private void MainWindow_StateChanged(object sender, EventArgs e)
        {
            if (WindowState == WindowState.Minimized && notifyIcon != null)
            {
                Hide();
                notifyIcon.Visible = true;
            }
        }

        private void RestoreFromTray()
        {
            Show();
            WindowState = WindowState.Normal;
            Activate();
            if (notifyIcon != null) notifyIcon.Visible = false;
        }

        // ------------------------------------------------------------------
        //  Cierre
        // ------------------------------------------------------------------
        private unsafe void MainWindow_Closed(object sender, EventArgs e)
        {
            isStreaming = false;
            isFrozen = false;
            SavePreferences();

            if (clientTimer != null) clientTimer.Stop();
            if (tiltTimer != null) tiltTimer.Stop();
            KinectSensor.KinectSensors.StatusChanged -= KinectSensors_StatusChanged;

            if (kinectSensor != null)
            {
                kinectSensor.ColorFrameReady -= KinectSensor_ColorFrameReady;
                try { kinectSensor.Stop(); } catch { }
            }

            if (shmPtr != null)
            {
                *(int*)(shmPtr + OffActive) = 0;
                shmPtr = null;
                if (memoryAccessor != null) memoryAccessor.SafeMemoryMappedViewHandle.ReleasePointer();
            }
            if (memoryAccessor != null) memoryAccessor.Dispose();
            if (sharedMemory != null) sharedMemory.Dispose();

            if (notifyIcon != null) { notifyIcon.Visible = false; notifyIcon.Dispose(); }
        }
    }
}
