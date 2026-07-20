# Sistema de Monitoreo IoT para Máquina de Pinball.
**Desarrollado para:** Nakamura Logistic | Versión 1.0 (Etapa Final)

Este repositorio contiene el firmware definitivo y la documentación técnica para el sistema automatizado de auditoría, control de flujo de efectivo y seguridad perimetral basado en el microcontrolador ESP32.

## 📦 Estructura del Repositorio
* **`/FIRMWARE`**: Contiene el código fuente principal (`firmware_pinball.ino`) optimizado para producción.
* **Archivos raíz**: Historial de desarrollo y evolución por etapas.

## 🛠️ Características Técnicas Implementadas
* **Adquisición de Ingresos de Alta Precisión:** Lectura de pulsos del monedero mediante Interrupciones de Hardware (ISRs) con control de rebotes.
* **Lógica Local y Balanceo:** Algoritmo integrado para el accionamiento y validación de entrega de la tolva (Hopper).
* **Motor de Seguridad Integrado:** Algoritmo anti-fraude para detección de patrones anormales (intento de "Pesca") y monitoreo continuo del pin de intrusión ("Gabinete Abierto").
* **Telemetría Asíncrona Eficiente:** Conexión WiFi y comunicación HTTP/REST sin bloqueo del procesador y con optimización manual de cadenas JSON (libre de fragmentación en la memoria Heap por remoción de ArduinoJson).

## 🔌 Asignación de Pines (Pinout ESP32)
* **Entrada Monedero (Pulsos):** Pin 4
* **Botón de Validación (Hopper):** Pin 19
* **Sensor de Intrusión (Gabinete):** Pin 18
