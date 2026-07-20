#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// Activación del entorno de simulación para pruebas de estrés de señal
#define MODO_TEST_INYECTOR 

#ifdef MODO_TEST_INYECTOR
  const int PIN_INYECTOR_TEST = 5;       // Pin de salida física para generar los pulsos automáticos de prueba
  unsigned long tiempoUltimoPulsoInyector = 0; // Registro de tiempo para controlar el parpadeo de la onda cuadrada
  unsigned long tiempoInicioRafagaTest = 0;   // Temporizador base para disparar la ráfaga cada 30 segundos
  bool rafagaTestActiva = false;         // Bandera de control para saber si la inyección está en proceso
  int pulsosInyectadosContador = 0;      // Contador de ciclos completados dentro de la ráfaga actual
#endif

// Asignación de Pines de Hardware
const int PIN_MONEDA = 15;          // Entrada con interrupción asignada al sensor lector de monedas
const int PIN_APAGAR_LED = 18;      // Entrada del botón físico que valida la entrega manual del dispositivo
const int PIN_PARO_EMERGENCIA = 19; // Entrada del botón de paro total para congelar la máquina ante fallas
const int PIN_HOPPER_MOTOR = 23;    // Salida digital dedicada a la activación eléctrica del motor del Hopper
const int PIN_HOPPER_SENSOR = 17;   // Entrada digital del sensor óptico/mecánico de entrega del Hopper
const int PIN_LED_DEVOLUCION = 2;   // Salida digital del indicador local de retorno de saldo excedente
const int PIN_LED_IOT = 16;         // Salida digital para el indicador visual comandado remotamente vía REST

// Constantes de Tiempos y Filtros
const unsigned long T_INTERVALO = 50;   // Tiempo de rechazo en milisegundos para el filtro de rebotes por hardware
volatile bool monedaDetectada = false;   // Estado lógico compartido con la ISR para señalar un pulso entrante
volatile unsigned long ultimoPulsoValido = 0; // Registro del milisegundo exacto de la última lectura aceptada

// Variables de Control Interno del Sistema
int totalEntregadoHistorico = 0;       // Contador acumulativo de monedas ingresadas con éxito al contenedor
int totalDevueltoHistorico = 0;        // Contador acumulativo de transacciones de cambio efectuadas
bool esperandoValidacionHopper = false; // Bandera de espera activa mientras el motor despacha el producto
String statusSeguridad = "CLEAN";      // Variable de estado que gobierna el motor de contingencias del firmware

// Temporizadores Asíncronos gestionados con millis()
unsigned long tiempoInicioPago = 0;                // Guarda el instante en que arrancó el motor para medir fallas
const unsigned long TIMEOUT_HOPPER_SEGURIDAD = 4000; // Límite de tiempo máximo permitido para dispensar una unidad

unsigned long tiempoUltimoFraudeAutomatico = 0;       // Controla el intervalo entre simulaciones preventivas de fraude
const unsigned long INTERVALO_FRAUDE_AUTOMATICO = 60000; // Tiempo de espera (60 segundos) para simular un intento de intrusión

unsigned long tiempoInicioBloqueo = 0;       // Registra el inicio del estado penalizado por alerta de seguridad
const unsigned long DURACION_BLOQUEO = 3000;  // Tiempo en milisegundos que el sistema permanece congelado

bool ejecucionDevolucionActiva = false;       // Bandera de control para mantener encendido el indicador de cambio
unsigned long tiempoInicioDevolucion = 0;     // Almacena el milisegundo de inicio de la alerta de devolución
const unsigned long DURACION_PULSO_DEVOLUCION = 500; // Duración del destello luminoso de devolución

String cadenaComando = ""; // Almacenamiento auxiliar para cadenas de comunicación

// Parámetros de Red y Configuración del Servicio REST
const char* SSID_WIFI = "Wokwi-GUEST"; // Identificador de la red inalámbrica simulada
const char* PASS_WIFI = "";            // Clave de seguridad de la red virtual
unsigned long tiempoUltimoIntentoWiFi = 0; // Temporizador para evitar llamadas continuas a la conexión de red
const unsigned long INTERVALO_RECONEXION_WIFI = 5000; // Tiempo de espera entre reintentos de enganche de red

const char* URL_BASE = "http://localhost:3000/api"; // Dirección de destino de los endpoints del backend local

unsigned long tiempoUltimoEnvioTelemetria = 0; // Temporizador de control para la publicación de datos
const unsigned long INTERVALO_TELEMETRIA = 1500; // Frecuencia de envío de datos del POST (1.5 segundos)

unsigned long tiempoUltimoGET = 0;  // Temporizador de control para la solicitud de comandos remotos
const unsigned long INTERVALO_GET = 2500; // Frecuencia de consulta del estado de actuadores (2.5 segundos)

// Rutina de Servicio de Interrupción (ISR) alojada en IRAM para máxima velocidad
void IRAM_ATTR verificarMonedaISR() {
    unsigned long t_actual = millis(); // Captura instantánea del tiempo de ejecución actual
    
    // Si el hardware está bloqueado esperando confirmación o con fraudes, ignora la señal física
    if (esperandoValidacionHopper && statusSeguridad != "CLEAN") return;

    // Validación del filtro de ventana de tiempo contra ruidos eléctricos
    if (t_actual - ultimoPulsoValido >= T_INTERVALO) {
        monedaDetectada = true;        // Avisa al bucle principal que hay un evento físico por procesar
        ultimoPulsoValido = t_actual;  // Actualiza la marca de tiempo del último evento filtrado
    }
}

// Función encargada de imprimir reportes formateados en el terminal
void imprimirReporteEstructurado(String evento) {
    Serial.println("\n=========================================");
    Serial.print("EVENTO: "); Serial.println(evento);
    Serial.println("-----------------------------------------");
    Serial.print(" -> TOTAL EN CAJA HISTÓRICO: $"); Serial.println(totalEntregadoHistorico);
    Serial.print(" -> TOTAL DEVOLUCIÓN HISTÓRICO: $"); Serial.println(totalDevueltoHistorico);
    Serial.print(" -> Estado del Hopper Motor:  "); Serial.println(digitalRead(PIN_HOPPER_MOTOR) == HIGH ? "ENCENDIDO" : "APAGADO");
    Serial.print(" -> Security Engine Status:   "); Serial.println(statusSeguridad);
    Serial.println("=========================================");
}

// Máquina de estados no bloqueante para asegurar conectividad WiFi permanente
void gestionarWiFiAsincrono(unsigned long tiempoActual) {
    static wl_status_t ultimoEstadoWiFi = WL_NO_SHIELD; // Mantiene el registro del estado de red previo
    wl_status_t estadoActualWiFi = WiFi.status();       // Consulta el estado eléctrico actual del módulo inalámbrico

    // Informa por consola únicamente si se presenta un cambio de estado en la red
    if (estadoActualWiFi != ultimoEstadoWiFi) {
        if (estadoActualWiFi == WL_CONNECTED) {
            Serial.print("\n[WIFI] ¡Conectado con éxito! IP Asignada: ");
            Serial.println(WiFi.localIP());
        } else if (estadoActualWiFi == WL_DISCONNECTED) {
            Serial.println("\n[WIFI] Desconectado de la red.");
        }
        ultimoEstadoWiFi = estadoActualWiFi; // Actualiza el registro persistente del estado
    }

    // Intento periódico de enganche si la interfaz se encuentra caída
    if (estadoActualWiFi != WL_CONNECTED) {
        if (tiempoActual - tiempoUltimoIntentoWiFi >= INTERVALO_RECONEXION_WIFI) {
            tiempoUltimoIntentoWiFi = tiempoActual; // Resetea el reloj de control de intentos
            Serial.println("[WIFI] Reintentando conectar a Wokwi-GUEST...");
            WiFi.begin(SSID_WIFI, PASS_WIFI); // Dispara la solicitud de asociación de red
        }
    }
}

// Transmisión asíncrona de datos del sistema mediante peticiones HTTP POST
void enviarTelemetriaREST(unsigned long tiempoActual) {
    if (tiempoActual - tiempoUltimoEnvioTelemetria >= INTERVALO_TELEMETRIA) {
        tiempoUltimoEnvioTelemetria = tiempoActual; // Actualiza el temporizador del POST
        
        if (WiFi.status() == WL_CONNECTED) { // Valida que exista canal de comunicación activo
            HTTPClient http;
            http.begin(String(URL_BASE) + "/sensores"); // Inicializa el destino HTTP del endpoint
            http.addHeader("Content-Type", "application/json"); // Define el formato de datos transportado
            
            // Construcción manual de la cadena JSON con las variables del firmware
            String jsonPayload = "{\"caja_historica\":" + String(totalEntregadoHistorico) + 
                                 ",\"total_devuelto\":" + String(totalDevueltoHistorico) + 
                                 ",\"status_seguridad\":\"" + statusSeguridad + "\"}";
            
            Serial.print("[HTTP POST] Enviando datos... ");
            int codigoRespuesta = http.POST(jsonPayload); // Ejecuta la transmisión y recupera el código HTTP
            Serial.print("Código Servidor: "); Serial.println(codigoRespuesta);
            http.end(); // Libera los recursos de red de la conexión actual
        } else {
            Serial.println("[HTTP POST] Saltado: WiFi desconectado.");
        }
    }
}

// Consulta asíncrona de instrucciones externas mediante peticiones HTTP GET
void consultarEstadoRemotoGET(unsigned long tiempoActual) {
    if (tiempoActual - tiempoUltimoGET >= INTERVALO_GET) {
        tiempoUltimoGET = tiempoActual; // Actualiza el temporizador del GET
        
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            http.begin(String(URL_BASE) + "/actuadores/led1"); // Inicializa el endpoint de lectura
            
            Serial.print("[HTTP GET] Consultando API... ");
            int codigoRespuesta = http.GET(); // Lanza la petición de lectura
            Serial.print("Código Servidor: "); Serial.println(codigoRespuesta);
            
            if (codigoRespuesta == 200) { // Valida que el servidor responda correctamente
                String respuesta = http.getString(); // Extrae la respuesta en formato texto
                Serial.println("[HTTP GET] Respuesta JSON: " + respuesta);
                
                // Análisis manual de subcadenas para detectar la orden de bloqueo remoto
                if (respuesta.indexOf("\"encendido\":true") != -1) {
                    digitalWrite(PIN_LED_IOT, HIGH); // Refleja el estado físico en el indicador visual
                    
                    // Transiciona al estado de bloqueo si el sistema no estaba previamente condicionado
                    if (statusSeguridad != "REMOTE_LOCKED") {
                        statusSeguridad = "REMOTE_LOCKED";
                        digitalWrite(PIN_HOPPER_MOTOR, LOW);     // Apaga el actuador de forma inmediata por seguridad
                        digitalWrite(PIN_LED_DEVOLUCION, LOW);   // Corta las señales locales secundarias
                        esperandoValidacionHopper = false;       // Cancela cualquier proceso de cobro en marcha
                        imprimirReporteEstructurado("BLOQUEO_DESDE_API_REST_ACTIVADO");
                    }
                } else {
                    digitalWrite(PIN_LED_IOT, LOW); // Apaga el indicador al recibir la instrucción contraria
                    if (statusSeguridad == "REMOTE_LOCKED") {
                        statusSeguridad = "CLEAN";  // Libera las restricciones operativas del sistema
                        imprimirReporteEstructurado("SISTEMA_RESTABLECIDO_DESDE_API_REST");
                    }
                }
            }
            http.end(); // Finaliza el objeto de comunicación HTTP
        } else {
            Serial.println("[HTTP GET] Saltado: WiFi sin conexión.");
        }
    }
}

// Procesamiento y lógica contable interna ante despachos válidos
void verificarYRegistrarEntrega() {
    totalEntregadoHistorico += 1;      // Incrementa el acumulador persistente de la bóveda
    esperandoValidacionHopper = false; // Da por terminado el ciclo de monitoreo de despacho activo
    
    // Regla de negocio: Generación de un ciclo de cambio automático cada 5 transacciones
    if (totalEntregadoHistorico % 5 == 0) {
        totalDevueltoHistorico += 1;             // Suma una unidad al registro histórico de cambios
        ejecucionDevolucionActiva = true;        // Activa la máquina de estados del pulso del LED
        tiempoInicioDevolucion = millis();       // Toma el tiempo de inicio para controlar la duración
        digitalWrite(PIN_LED_DEVOLUCION, HIGH);  // Enciende físicamente la luz piloto de cambio
        imprimirReporteEstructurado("DEVOLUCION_5_PESOS_ACTIVADA");
    } else {
        imprimirReporteEstructurado("ENTREGA_CONFIRMADA");
    }
}

// Simulador automático de señales periódicas de estrés para depuración
void gestionarInyectorTest(unsigned long tiempoActual) {
#ifdef MODO_TEST_INYECTOR
    // Lanzamiento automático de ráfagas cíclicas espaciadas cada 30 segundos
    if (!rafagaTestActiva && (tiempoActual - tiempoInicioRafagaTest >= 30000)) {
        rafagaTestActiva = true;
        tiempoInicioRafagaTest = tiempoActual;
        pulsosInyectadosContador = 0;
        pinMode(PIN_INYECTOR_TEST, OUTPUT); // Conmutación de pin a baja impedancia para controlar la línea
        Serial.println("\n[INYECTOR TEST] ¡Iniciando ráfaga de alta frecuencia para estresar la ISR!");
    }

    if (rafagaTestActiva) {
        // Generador de onda cuadrada simétrica operando a un intervalo estable de 4ms (~125Hz)
        if (tiempoActual - tiempoUltimoPulsoInyector >= 4) {
            tiempoUltimoPulsoInyector = tiempoActual; // Actualiza el contador del pulso de prueba
            int estadoActual = digitalRead(PIN_INYECTOR_TEST);
            digitalWrite(PIN_INYECTOR_TEST, !estadoActual); // Realiza la inversión eléctrica del estado anterior
            
            if (estadoActual == LOW) {
                pulsosInyectadosContador++; // Registra únicamente los flancos descendentes simulados
            }

            // Cierre preventivo de la rutina de prueba al alcanzar el límite de 15 pulsos
            if (pulsosInyectadosContador >= 15) {
                rafagaTestActiva = false;
                pinMode(PIN_INYECTOR_TEST, INPUT_PULLUP); // Pasa a alta impedancia para liberar la línea compartida
                Serial.print("[INYECTOR TEST] Ráfaga finalizada. Pulsos detectados en este ciclo.");
            }
        }
    }
#endif
}

// Configuración inicial de periféricos, interrupciones y subsistemas de red
void setup() {
    Serial.begin(115200); // Inicialización del canal serial de telemetría local a 115200 bps
    
    // Declaración de modos operativos de la interfaz de pines GPIO del ESP32
    pinMode(PIN_MONEDA, INPUT_PULLUP);
    pinMode(PIN_APAGAR_LED, INPUT_PULLUP);
    pinMode(PIN_PARO_EMERGENCIA, INPUT_PULLUP); 
    pinMode(PIN_HOPPER_SENSOR, INPUT);  
    pinMode(PIN_LED_DEVOLUCION, OUTPUT);
    pinMode(PIN_LED_IOT, OUTPUT); 
    pinMode(PIN_HOPPER_MOTOR, OUTPUT);
    
    // Fijación del estado eléctrico inicial apagado seguro para actuadores
    digitalWrite(PIN_HOPPER_MOTOR, LOW); 
    digitalWrite(PIN_LED_DEVOLUCION, LOW);
    digitalWrite(PIN_LED_IOT, LOW);

#ifdef MODO_TEST_INYECTOR
    pinMode(PIN_INYECTOR_TEST, INPUT_PULLUP); // Mantiene aislada la línea del inyector en el arranque
#endif
    
    // Vinculación de la ISR al pin detector en modo de flanco de bajada (FALLING)
    attachInterrupt(digitalPinToInterrupt(PIN_MONEDA), verificarMonedaISR, FALLING);
    
    WiFi.mode(WIFI_STA); // Inicialización del controlador inalámbrico en modo Estación (Cliente)
    Serial.println("[WIFI] Inicializando conexión a Wokwi-GUEST...");
    WiFi.begin(SSID_WIFI, PASS_WIFI); // Primer disparo asíncrono de enlace de red
    
    imprimirReporteEstructurado("SISTEMA_INICIALIZADO");
}

// Bucle principal de ejecución no bloqueante basado en eventos de tiempo
void loop() {
    unsigned long tiempoActual = millis(); // Captura temporal unificada para sincronizar todas las tareas

    gestionarInyectorTest(tiempoActual); // Lógica activa del emulador de pulsos en background

    // 1. Escaneo Prioritario de Entradas Críticas y Botones de Control
    int estadoActualParo = digitalRead(PIN_PARO_EMERGENCIA); 
    static int estadoAnteriorParo = HIGH; // Almacenamiento estático del estado del botón en el ciclo previo
    static unsigned long tiempoUltimoCambioParo = 0; // Filtro de rebotes por software exclusivo del botón de paro

    // Detección de flanco de bajada válido con un debounce de seguridad de 200ms
    if (estadoActualParo == LOW && estadoAnteriorParo == HIGH && (tiempoActual - tiempoUltimoCambioParo > 200)) { 
        tiempoUltimoCambioParo = tiempoActual;
        
        // Lógica tipo Toggle: Conmutación bidireccional entre bloqueo y desbloqueo total
        if (statusSeguridad != "EMERGENCY_STOPPED") {
            statusSeguridad = "EMERGENCY_STOPPED";
            digitalWrite(PIN_HOPPER_MOTOR, LOW);     // Apagado físico inmediato del motor principal
            digitalWrite(PIN_LED_DEVOLUCION, LOW);   // Apagado del indicador de cambio
            esperandoValidacionHopper = false;       // Rompe la rutina de despacho activa
            ejecucionDevolucionActiva = false;       // Apaga los procesos intermedios
            imprimirReporteEstructurado("PARO_TOTAL_FORZADO");
        } else {
            statusSeguridad = "CLEAN";               // Restaura las condiciones iniciales del sistema
            monedaDetectada = false;                 // Limpia registros espurios de monedas retenidas
            tiempoUltimoFraudeAutomatico = tiempoActual; // Resetea el intervalo del temporizador de seguridad
            imprimirReporteEstructurado("SISTEMA_LIBERADO");
        }
    }
    estadoAnteriorParo = estadoActualParo; // Actualiza la persistencia del estado físico

    // Monitoreo del botón auxiliar de cancelación manual del proceso de validación
    if (digitalRead(PIN_APAGAR_LED) == LOW && esperandoValidacionHopper) {
        digitalWrite(PIN_HOPPER_MOTOR, LOW); // Detiene la marcha forzada del motor
        verificarYRegistrarEntrega();        // Fuerza la contabilidad manual del evento
    }

    // 2. Orquestador de Eventos Locales (Lógica del Dispensador)
    if (statusSeguridad == "CLEAN") {
        // Recepción y procesamiento del evento de moneda disparado por la ISR
        if (monedaDetectada) {
            monedaDetectada = false;           // Libera la bandera para futuras lecturas externas
            esperandoValidacionHopper = true;  // Abre la ventana de control de despacho
            tiempoInicioPago = tiempoActual;   // Fija el límite de tiempo para evaluar la entrega
            digitalWrite(PIN_HOPPER_MOTOR, HIGH); // Activa mecánicamente el motor del Hopper
            imprimirReporteEstructurado("MONEDA_ENTRANTE_MOTOR_ON");
        }

        // Bucle de supervisión del proceso de salida física del producto
        if (esperandoValidacionHopper) {
            // Confirmación exitosa de paso por el sensor del Hopper
            if (digitalRead(PIN_HOPPER_SENSOR) == HIGH) {
                digitalWrite(PIN_HOPPER_MOTOR, LOW); // Apaga de inmediato el motor
                verificarYRegistrarEntrega();        // Registra contablemente la salida
            }
            // Control de anomalías por estancamiento mecánico (Timeout superado)
            else if (tiempoActual - tiempoInicioPago >= TIMEOUT_HOPPER_SEGURIDAD) {
                digitalWrite(PIN_HOPPER_MOTOR, LOW); // Apaga el motor para evitar daños eléctricos
                esperandoValidacionHopper = false;   // Cancela la transacción inconclusa
                imprimirReporteEstructurado("TIMEOUT_HOPPER_SIN_MONEDAS_ERROR");
            }
        }
    }

    // Control asíncrono del destello luminoso para la entrega de cambio
    if (ejecucionDevolucionActiva && (tiempoActual - tiempoInicioDevolucion >= DURACION_PULSO_DEVOLUCION)) {
        digitalWrite(PIN_LED_DEVOLUCION, LOW); // Corta la excitación del pin indicador
        ejecucionDevolucionActiva = false;    // Cierra la subrutina del pulso
    }

    // Disparador preventivo automático para simulación y pruebas internas del Security Engine
    if (statusSeguridad == "CLEAN" && !esperandoValidacionHopper) {
        if (tiempoActual - tiempoUltimoFraudeAutomatico >= INTERVALO_FRAUDE_AUTOMATICO) {
            tiempoUltimoFraudeAutomatico = tiempoActual;
            statusSeguridad = "FRAUD_DETECTED";     // Transiciona el firmware al estado de sospecha
            digitalWrite(PIN_HOPPER_MOTOR, LOW);     // Protege los actuadores principales
            digitalWrite(PIN_LED_DEVOLUCION, LOW);   // Corta las luces del panel
            tiempoInicioBloqueo = tiempoActual;      // Inicia el cronómetro de la penalización
            imprimirReporteEstructurado("ALERTA_FRAUDE_SIMULADO");
        }
    }

    // Auto-recuperación programada tras el paso de la ventana de penalización por fraude
    if (statusSeguridad == "FRAUD_DETECTED" && (tiempoActual - tiempoInicioBloqueo >= DURACION_BLOQUEO)) {
        statusSeguridad = "CLEAN";                  // Restablece el firmware a operación limpia
        monedaDetectada = false;                     // Descarta cualquier pulso acumulado en el bloqueo
        tiempoUltimoFraudeAutomatico = tiempoActual; // Reinicia el temporizador de fraude periódico
    }

    // 3. Gestión y Sincronización Asíncrona de Tareas de Red
    gestionarWiFiAsincrono(tiempoActual);
    enviarTelemetriaREST(tiempoActual);
    consultarEstadoRemotoGET(tiempoActual);
}
