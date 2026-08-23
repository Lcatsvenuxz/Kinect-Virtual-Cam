-Kinect Virtual Cam - Zero Scaling

Un controlador DirectShow de alto rendimiento y una interfaz C# que convierten el sensor Microsoft Kinect v1 en una cámara web nativa para Windows, enfocada en la estabilidad técnica y el rendimiento de la CPU.

-- Características Principales

* Cero Escalado: Arquitectura estricta a 480p nativos (640x480 @ 30 FPS). La imagen no se estira ni se rellena con basura de memoria, garantizando compatibilidad con WebRTC sin crasheos (creo).
* Control de Hardware en Tiempo Real: Ajuste del motor de inclinación físico de la Kinect directamente desde la interfaz gráfica.
* Modos de Visión Dual: Cambio en rapido entre cámara a color (RGB) y cámara Infrarroja (IR).
* Bajo Consumo: Aislamiento de hardware vía IPC y candados de subprocesos que mantienen el impacto en el procesador prácticamente nulo.
* Corrección de Orientación: Inversión de ejes (Flip Horizontal/Vertical) nativa para solucionar el efecto espejo.

-- Requisitos de Instalación

* Hardware: Microsoft Kinect v1 (Xbox 360) con su adaptador de corriente USB.
* Sistema: Windows 10 / Windows 11 (Exclusivo 64-bits).
* Dependencia Crítica: Tener instalado los drivers de la kinect (Kinect for Windows SDK v1.8 https://www.microsoft.com/en-us/download/details.aspx?id=40278 )

-- Limitaciones que me di cuenta

* Discord Desktop App: ni idea porque no funciona con la app de escritorio de Discord jajsalu2. 
* Soluciones: Se puede usar Discord Web, o puenteando la señal final con OBS Virtual Cam, NVIDIA Broadcast o lo que quieras, ese ya no es mi problema xd.
