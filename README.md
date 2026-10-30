-Kinect Virtual Cam - Zero Scaling

Un controlador DirectShow de alto rendimiento y una interfaz C# que convierten el sensor Microsoft Kinect v1 en una cámara web nativa para Windows, enfocada en la estabilidad técnica y el rendimiento de la CPU.

-- Características Principales

* Cero Escalado: Arquitectura estricta en 4:3 nativos tanto en 480p (640x480 @ 30 FPS) como en 960p (1280x960 @ 12 FPS). La imagen no se estira ni se deforma, garantizando compatibilidad limpia con WebRTC.
* Soporte Multiarquitectura: Filtro compilado y registrado tanto en x64 como en x86 (Win32), por lo que funciona nativamente en OBS, Meet y Discord de escritorio, etc.
* Modo Congelar Imagen: Permite pausar el video en vivo alv.
* Control de Hardware en Tiempo Real: Ajuste del motor de inclinación físico de la Kinect directamente desde la interfaz gráfica.
* Modos de Visión Dual: Cambio entre cámara a color y cámara Infrarroja.
* Personalización: Selector dinámico de idioma (Español / Inglés) y selector de temas de interfaz.
* Bajo Consumo: Canalización por memoria compartida atómica IPC Seqlock que mantiene el impacto en el procesador prácticamente nulo.
* Corrección de Orientación: Inversión de ejes Flip Horizontal/Vertical.

-- Requisitos de Instalación

* Hardware: Microsoft Kinect v1 (Xbox 360) con su adaptador de corriente USB.
* Sistema: Windows 10 / Windows 11 (Exclusivo 64-bits).
* Dependencias: Tener instalado el runtime de la kinect (Kinect for Windows SDK v1.8: https://www.microsoft.com/en-us/download/details.aspx?id=40278 ).

-- Limitaciones que me di cuenta

* WhatsApp Desktop: No funciona ahí xd
