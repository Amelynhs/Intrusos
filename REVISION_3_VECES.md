# Revision 3 veces antes de entregar

## Revision 1: estructura

- Firmware presente.
- App Expo presente.
- Servicio HTTP de ESP32 presente.
- Cliente Supabase presente.
- SQL de esquema y migracion presente.
- README y defensa presentes.

## Revision 2: sintaxis y consistencia estatica

Resultado: **33/33 checks automaticos PASS**.

Se verificaron:
- balance de parentesis, llaves y corchetes del firmware;
- endpoints requeridos;
- tareas FreeRTOS;
- mutex de camara/frame/cola;
- cola de eventos;
- proteccion contra duplicados;
- estados SMTP;
- contratos Expo-Supabase;
- contrato del stream en puerto 81.

Tambien se probo la logica de horarios para:
- franja normal;
- franja que cruza medianoche;
- franja 24 horas.

## Revision 3: integracion

Se verifico que coincidan:
- `DEVICE_ID` entre firmware y app;
- IP del AP;
- endpoints de control;
- puerto del stream;
- campos de horarios;
- estados de eventos;
- historial de Supabase;
- deteccion local independiente de Expo;
- cola cuando STA/Internet falla;
- credenciales SMTP mediante contraseña de aplicacion.

## Lo que no puede certificarse sin tu hardware

No se puede afirmar que la compilacion final ni el arranque fisico sean correctos sin la placa ESP32-S3-CAM exacta conectada. Tampoco se puede verificar el pinout real de una placa cuyo fabricante/modelo no haya sido identificado.
