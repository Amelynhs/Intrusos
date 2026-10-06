# Parte 5 - Integracion completa, pruebas y defensa

Esta carpeta consolida las Partes 3 y 4 y agrega correcciones de integracion, pruebas reproducibles y material para la defensa.

## 1. Que se corrigio

### ESP32-S3 CAM
- La cola de intrusiones ahora distingue entre registro en Supabase y envio de correo.
- Un reintento no crea automaticamente una segunda fila para el mismo `epoch`.
- Si SMTP no esta configurado, el sistema **no** afirma que el correo fue enviado.
- Si SMTP falla, el evento permanece registrado y se reintenta; despues de 5 fallos de correo se conserva el evento en Supabase y se retira de la cola local para no bloquear eventos posteriores.
- Se elimino una asignacion duplicada innecesaria en la inicializacion del estado de horarios.
- Se mantiene la deteccion local aunque Internet falle.

### Expo Go
- Reconecta el estado de la ESP32 cada 2.5 s.
- Reintenta el stream si WebView reporta un error.
- Al volver la app a primer plano fuerza una comprobacion del estado y una recarga del stream.
- Mantiene historial y horarios en Supabase.
- No depende de Expo Go para que la deteccion fisica ocurra: esa logica sigue en el ESP32.

### Supabase
- Se agrega una restriccion unica por dispositivo + dia para horarios.
- Se agrega un indice para buscar eventos por `metadata.epoch`.

## 2. Configuracion

1. En Supabase ejecuta `supabase/migration_part5.sql`.
2. En el firmware cambia `STA_SSID` y `STA_PASSWORD`.
3. Configura el correo en el firmware.
4. Para Gmail usa una **contraseña de aplicacion**, nunca tu contraseña normal.
5. Instala `ESP Mail Client` desde el Library Manager del Arduino IDE.
6. Instala la app:

```bash
npm install
npx expo start --lan
```

7. Conecta el telefono al AP `Intrusos-ESP32S3-CAM` si estas usando la red local de la placa.
8. En la app usa `http://192.168.4.1` como IP inicial.

## 3. Prueba completa recomendada

### A. Arranque
- Abrir Serial Monitor a 115200.
- Comprobar que la camara inicializa.
- Comprobar AP `Intrusos-ESP32S3-CAM`.
- Comprobar IP AP `192.168.4.1`.
- Si se configuro STA, comprobar que obtiene IP.
- Abrir `/status` desde el telefono.

### B. Video
- Abrir el stream desde la app.
- Confirmar que se actualizan fotogramas.
- Desconectar/reconectar el stream y comprobar el reintento.

### C. Horario
- Configurar un horario de prueba de 2-3 minutos.
- Probar una franja normal, por ejemplo 14:00 -> 14:05.
- Probar una franja que cruce medianoche, por ejemplo 22:00 -> 06:00.
- Verificar `scheduleActive` y el cambio automatico de estado.

### D. Intrusion
1. Sistema desarmado + movimiento -> no debe generar INTRUSION.
2. Sistema armado + movimiento -> debe pasar a INTRUSION.
3. Debe aumentar `intrusionCount`.
4. Debe aparecer un evento en `intrusion_events` cuando Internet este disponible.
5. Con SMTP configurado debe llegar el correo.

### E. Perdida de Internet
- Mantener el telefono conectado al AP.
- Cortar Internet del router.
- Generar movimiento con el sistema armado.
- El ESP32 debe seguir detectando localmente.
- Restablecer Internet.
- El evento pendiente debe enviarse a Supabase y, con SMTP listo, intentar el correo.

### F. Perdida de ESP32
- Apagar la placa.
- La app debe mostrar `ESP32-S3 SIN CONEXION`.
- Encenderla otra vez.
- La app debe recuperar el estado sin reiniciarse manualmente.

## 4. Tres revisiones realizadas antes de entregar

### Revision 1 - Estructura
- Archivos de firmware, Expo, servicios y SQL presentes.
- Dependencias coherentes entre `package.json`, `App.js` y servicios.
- IDs de dispositivo consistentes.

### Revision 2 - Sintaxis/estatica
- Conteo balanceado de `()`, `{}` y `[]` en el firmware.
- Comprobacion de sintaxis JavaScript en archivos sin JSX.
- Busqueda de placeholders y TODOs.
- Revision de rutas `/status`, `/arm`, `/disarm`, `/clear`, `/capture`, `/time`, `/schedule`, `/stream`.

### Revision 3 - Integracion logica
- ESP32 -> Supabase: registro de eventos y horarios.
- ESP32 -> SMTP: alerta y reintentos.
- Expo -> ESP32: estado, armado, desarmado, hora y video.
- Expo -> Supabase: historial y horarios.
- Perdida de Internet: deteccion local + cola.
- Perdida de ESP32: estado de desconexion + reconexion.
- Horarios que cruzan medianoche.

## 5. Limite importante de la verificacion

No se puede certificar desde el computador que el firmware arranca correctamente en **tu placa fisica** ni que el pinout corresponde a tu modelo exacto de ESP32-S3-CAM. El nombre comercial `ESP32-S3-CAM` no identifica un unico mapa de pines.

La prueba final de hardware requiere tu placa, su camara OV2640 y el metodo de programacion que estes usando.

Tampoco se debe presentar Expo Go como un verdadero Android Foreground Service. La deteccion critica se mantiene en el ESP32; si el profesor exige literalmente un servicio nativo persistente en segundo plano, eso requiere un development build y codigo nativo adicional.

## 6. Defensa tecnica

La defensa debe poder explicar:
- por que la deteccion esta en el ESP32;
- diferencia entre AP y STA;
- HTTP y MJPEG;
- por que el stream usa un puerto separado;
- mutexes y tareas FreeRTOS;
- cola de eventos y reintentos;
- NTP y sincronizacion manual;
- horarios normales y cruzando medianoche;
- RLS de Supabase;
- diferencia entre publishable key y secret key;
- por que una contraseña de aplicacion de Gmail no es la contraseña normal;
- que ocurre cuando se cae Internet;
- que ocurre cuando se apaga la ESP32;
- limitaciones reales de Expo Go en segundo plano.
