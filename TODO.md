Tasks:
  0- Main: Inicialmente se configurarán todos los puertos, sensores y módulos necesarios. Se analiza si hay una nueva actualización
  disponible y se actualiza en caso necesario.
	1- OTA: Se analizará periódicamente (periodo a definir) si existe una nueva actualización de firmware disponible,
  en cuyo caso se procedería a realizar una actualización OTA (Over The Air) con posibilidad de Rollback en caso de fallo
	2- Acelerómetro: Tarea que lee periódicamente (periodo a definir) el sensor integrado TriAxis y guarda los valores 
  obtenidos en sus variables correspondientes globales. Determinar si esta tarea realizará lecturas en modo ahorro de energía
  o con otra política.
	3- Micrófono: Tarea que analiza constantemente el sonido alrededor, a la espera de detectar algún comando (por definir),
  en cuyo caso se realizará la acción correspondiente.
	4- Wifi/Telemetría (batería, ubicación, movimiento, comandos…): Esta tarea se encarga de gestionar el uso del wifi según la política 
  de ahorro de energía implementada, y envía periódicamente los datos recogidos como telemetría hacia thingsboard.
	5- Pantalla: Tarea que gestiona la interacción del usuario con el dispositivo mediante la pantalla y el sensor táctil. 
  De normal la pantalla está apagada, pero cuando el usuario toca la pantalla, se mostrará la hora actual y se permitirá
  navegar por un menú (a definir lo que ofrece al usuario).
	6- Reloj/Alarmas: Tarea que mantiene un registro con todas las alarmas activas y emite las alarmas que se haya configurado
  por el usuario mediante alguna de las interfaces.
	7- BLE-Ubicación: Tarea que se conecta por bluetooth con el teléfono móvil del usuario y recibe la ubicación en tiempo real 
  para que esta sea emitida como telemetría.

Aplicación Móvil:
1- Compartir ubicación con el dispositivo
2- Conectarse con Thingsboard y recoger la telemetría y los análisis realizados.

Thingsboard:
	1- Recoger toda la telemetría enviada por el dispositivo
	2- Realizar análisis de los datos (alarmas, etc), manteniendo históricos
	3- Enviar a telegram

Bot Telegram:
	1- Recibir alarmas y en esencia ofrecer el mismo funcionamiento que la aplicación (menos la ubicación)
