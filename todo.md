# TODO - Proyecto VitaMove

## ⌚ Firmware ESP32 (LILYGO T-Watch 2020 V3)

* \[ \] **Task 0 - Main:**
  * \[ \] Configuración inicial de puertos, sensores y módulos.
  * \[ \] Comprobación de actualizaciones disponibles al arranque.

* \[ \] **Task 1 - OTA (Over The Air):**
  * \[ \] Definir periodo de comprobación de actualizaciones de firmware.
  * \[ \] Implementar sistema de actualización remota.
  * \[ \] Implementar mecanismo de *Rollback* en caso de fallo crítico.

* \[ \] **Task 2 - Acelerómetro (BMA423):**
  * \[ \] Definir periodo de lectura del sensor TriAxis.
  * \[ \] Guardar valores obtenidos en variables globales.
  * \[ \] Determinar e implementar política de ahorro de energía para estas lecturas.

* \[ \] **Task 3 - Micrófono (SPM1423HM4H-B):**
  * \[ \] Implementar lógica de grabación bajo demanda ("Push-to-talk").
  * \[ \] Integrar con la interfaz táctil: Iniciar grabación al tocar el botón, detener al volver a tocar.
  * \[ \] Procesar el audio resultante para identificar el comando dictado y asociar su acción.

* \[ \] **Task 4 - WiFi / Telemetría (Store & Forward):**
  * \[ \] Implementar gestor de conexión WiFi con prioridades (1º Red del hogar, 2º Punto de acceso del móvil compartiendo conexión).
  * \[ \] Empaquetar datos (batería, ubicación, movimiento, comandos).
  * \[ \] Enviar telemetría directamente hacia Thingsboard cuando haya conexión.
  * \[ \] Implementar sistema de cola/almacenamiento local (SPIFFS o LittleFS) para guardar telemetría si no hay conexión WiFi.
  * \[ \] Enviar el histórico almacenado al recuperar la conexión.

* \[ \] **Task 5 - Pantalla e Interfaz (ST7789V & FT6336):**
  * \[ \] Configurar suspensión de pantalla por defecto.
  * \[ \] Implementar despertar de pantalla mediante toque (interrupción).
  * \[ \] Mostrar hora actual.
  * \[ \] Diseñar menú de navegación simplificado (incluyendo el botón para activar el micrófono).

* \[ \] **Task 6 - Reloj / Alarmas (RTC PCF8563):**
  * \[ \] Mantener registro de alarmas activas.
  * \[ \] Ejecutar alarmas configuradas mediante altavoz y/o motor de vibración.

* \[ \] **Task 7 - BLE y Ubicación:**
  * \[ \] Establecer conexión Bluetooth Low Energy con el smartphone.
  * \[ \] Recibir coordenadas GPS en tiempo real para adjuntar a la telemetría.

* \[ \] **Task 8 - Gestión de Energía (AXP202):**
  * \[ \] Leer estado real de la batería y monitorizar carga/descarga.
  * \[ \] Implementar políticas de entrada en *Deep Sleep* o *Light Sleep* del ESP32.
  * \[ \] Apagar periféricos innecesarios cuando el sistema esté en reposo.

## 📱 Aplicación Móvil

* \[ \] Implementar servicio (preferiblemente en segundo plano) para compartir ubicación GPS por BLE con el reloj.
* \[ \] Mantener activo el punto de acceso WiFi (Hotspot) para dar conexión al reloj en exteriores.
* \[ \] Integrar conexión con Thingsboard para visualizar telemetría.
* \[ \] Mostrar análisis de datos procesados e históricos.
* \[ \] Desarrollar interfaz para que cuidadores gestionen el sistema (configurar horarios, recordatorios, alarmas).

## ☁️ Backend (Thingsboard)

* \[ \] Configurar ingesta de datos (recoger telemetría directa del ESP32).
* \[ \] Crear *Rule Chains* (algoritmos) para analizar los datos brutos e identificar alarmas.
* \[ \] Configurar persistencia e históricos de actividad.
* \[ \] Gestionar los datos que llegan con retraso (*timestamp* antiguo) desde la memoria offline del reloj.

## 🚀 Futuro / Backlog

* \[ \] **Bot de Telegram:**
  * \[ \] Integrar nodo de alerta en Thingsboard para enviar notificaciones a Telegram.
  * \[ \] Desarrollar interfaz paralela de comandos en el bot para simplificar la gestión por parte de los cuidadores.