using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.IO.MemoryMappedFiles;
using System.Threading;
using Microsoft.Kinect;

namespace KinectControllerApp
{
    public partial class MainWindow : Window
    {
        private KinectSensor kinectSensor;
        private MemoryMappedFile sharedMemory;
        private MemoryMappedViewAccessor memoryAccessor;
        private bool isStreaming = false;

        // El candado de rendimiento: 0 = Libre, 1 = Ocupado procesando un fotograma
        private int processingFlag = 0;

        private readonly int MAX_BUFFER_SIZE = 8 + (640 * 480 * 4);
        private byte[] pixelData;
        private byte[] finalRgbData;
        private byte[] ipcHeader;
        private WriteableBitmap bitmap;

        public MainWindow()
        {
            InitializeComponent();
            this.Loaded += MainWindow_Loaded;
            this.Closed += MainWindow_Closed;
        }

        private void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                sharedMemory = MemoryMappedFile.CreateOrOpen("KinectSharedMem", MAX_BUFFER_SIZE);
                memoryAccessor = sharedMemory.CreateViewAccessor();
                ipcHeader = new byte[8];
            }
            catch { MessageBox.Show("Error IPC."); }

            if (KinectSensor.KinectSensors.Count > 0)
            {
                kinectSensor = KinectSensor.KinectSensors[0];
                sliderTilt.ValueChanged += SliderTilt_ValueChanged;

                // Previsualización inicial (puede ser 960p o 480p según tu UI, aquí inicia estándar)
                kinectSensor.ColorStream.Enable(ColorImageFormat.RgbResolution640x480Fps30);
                kinectSensor.ColorFrameReady += KinectSensor_ColorFrameReady;
                kinectSensor.Start();
            }
        }

        private void SliderTilt_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (kinectSensor != null && kinectSensor.IsRunning)
            {
                try { kinectSensor.ElevationAngle = (int)e.NewValue; txtTiltAngle.Text = $"{(int)e.NewValue}°"; } catch { }
            }
        }

        private void BtnToggleInjection_Click(object sender, RoutedEventArgs e)
        {
            if (kinectSensor == null) return;

            if (!isStreaming)
            {
                // DETENEMOS EL SENSOR. SE ACABARON LOS JUEGOS.
                kinectSensor.Stop();

                // FORZAMOS LA RESOLUCIÓN A 480p INNEGOCIABLEMENTE.
                ColorImageFormat format = ColorImageFormat.RgbResolution640x480Fps30;
                if (rbColorIr.IsChecked == true) format = ColorImageFormat.InfraredResolution640x480Fps30;

                kinectSensor.ColorStream.Enable(format);
                kinectSensor.Start();

                isStreaming = true;
                btnToggleInjection.Content = "DETENER KINECT WEBCAM";
                btnToggleInjection.Background = System.Windows.Media.Brushes.DarkRed;
            }
            else
            {
                isStreaming = false;
                try
                {
                    if (memoryAccessor != null)
                    {
                        BitConverter.GetBytes(0).CopyTo(ipcHeader, 0); BitConverter.GetBytes(0).CopyTo(ipcHeader, 4);
                        memoryAccessor.WriteArray(0, ipcHeader, 0, 8);
                    }
                }
                catch { }
                btnToggleInjection.Content = "INICIAR KINECT WEBCAM";
                btnToggleInjection.Background = (System.Windows.Media.Brush)new System.Windows.Media.BrushConverter().ConvertFromString("#28A745");
            }
        }

        private void KinectSensor_ColorFrameReady(object sender, ColorImageFrameReadyEventArgs e)
        {
            // EL FRENO: Si la CPU todavía está procesando el fotograma anterior, descartamos este.
            // Esto evita que se saturen los hilos y baja el consumo de tu Ryzen 7 al mínimo.
            if (Interlocked.CompareExchange(ref processingFlag, 1, 0) != 0) return;

            using (ColorImageFrame colorFrame = e.OpenColorImageFrame())
            {
                if (colorFrame == null) { Interlocked.Exchange(ref processingFlag, 0); return; }

                int width = colorFrame.Width; int height = colorFrame.Height;
                bool isIr = (colorFrame.Format == ColorImageFormat.InfraredResolution640x480Fps30);
                int bytesPerPixel = isIr ? 2 : 4;

                if (pixelData == null || pixelData.Length != colorFrame.PixelDataLength)
                {
                    pixelData = new byte[colorFrame.PixelDataLength];
                    finalRgbData = new byte[width * height * 4];
                    bitmap = new WriteableBitmap(width, height, 96.0, 96.0, isIr ? PixelFormats.Gray16 : PixelFormats.Bgr32, null);
                    cameraFeed.Source = bitmap;
                }

                colorFrame.CopyPixelDataTo(pixelData);

                // Renderizado en la App
                bitmap.WritePixels(new Int32Rect(0, 0, width, height), pixelData, width * bytesPerPixel, 0);

                if (!isStreaming) { Interlocked.Exchange(ref processingFlag, 0); return; }

                bool flipV = chkFlipV.IsChecked == true; bool flipH = chkFlipH.IsChecked == true;

                // Enviar la carga matemática a un hilo de bajo nivel sin bloquear la interfaz
                ThreadPool.QueueUserWorkItem(_ => {
                    try
                    {
                        // OPTIMIZACIÓN EXTREMA: Si es RGB estándar y no hay efecto espejo, copiamos bloques de memoria directos. Es 100 veces más rápido que un bucle For.
                        if (!isIr && !flipH && !flipV)
                        {
                            Buffer.BlockCopy(pixelData, 0, finalRgbData, 0, pixelData.Length);
                        }
                        else
                        {
                            int dest = 0;
                            for (int y = 0; y < height; y++)
                            {
                                int srcY = flipV ? (height - 1 - y) : y;
                                for (int x = 0; x < width; x++)
                                {
                                    int srcX = flipH ? (width - 1 - x) : x;
                                    int srcIndex = (srcY * width + srcX) * bytesPerPixel;

                                    if (isIr)
                                    {
                                        byte intensity = pixelData[srcIndex + 1];
                                        finalRgbData[dest++] = intensity; finalRgbData[dest++] = intensity; finalRgbData[dest++] = intensity; finalRgbData[dest++] = 255;
                                    }
                                    else
                                    {
                                        finalRgbData[dest++] = pixelData[srcIndex]; finalRgbData[dest++] = pixelData[srcIndex + 1]; finalRgbData[dest++] = pixelData[srcIndex + 2]; finalRgbData[dest++] = 255;
                                    }
                                }
                            }
                        }

                        // Inyección de memoria compartida
                        if (memoryAccessor != null)
                        {
                            BitConverter.GetBytes(width).CopyTo(ipcHeader, 0); BitConverter.GetBytes(height).CopyTo(ipcHeader, 4);
                            memoryAccessor.WriteArray(0, ipcHeader, 0, 8);
                            memoryAccessor.WriteArray(8, finalRgbData, 0, finalRgbData.Length);
                        }
                    }
                    catch (Exception) { }
                    finally
                    {
                        // Siempre liberamos el candado al final para aceptar el siguiente fotograma
                        Interlocked.Exchange(ref processingFlag, 0);
                    }
                });
            }
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            isStreaming = false; if (kinectSensor != null) kinectSensor.Stop();
            if (memoryAccessor != null) memoryAccessor.Dispose(); if (sharedMemory != null) sharedMemory.Dispose();
        }
    }
}