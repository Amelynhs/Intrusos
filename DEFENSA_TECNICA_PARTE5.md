# Defensa tecnica - Parte 5

## Arquitectura

```text
                 INTERNET
                    |
          +---------+---------+
          |                   |
       Supabase            Gmail SMTP
          ^                   ^
          |                   |
      HTTPS REST          SMTP/TLS
          |                   |
      +---+-------------------+---+
      |       ESP32-S3 CAM         |
      |  camara + deteccion local  |
      |  horarios + cola + HTTP    |
      +-------------+--------------+
                    |
               Wi-Fi AP
                    |
               Expo Go
```

## Preguntas que debes poder responder

1. **¿Por que detectar movimiento en el ESP32?**
   Porque la deteccion critica no debe depender de que la app este abierta. El ESP32 puede seguir vigilando aunque el telefono se desconecte.

2. **¿Que hace AP y que hace STA?**
   AP crea la red local para que el telefono se conecte. STA conecta la ESP32 al router para Internet, NTP, Supabase y SMTP.

3. **¿Por que hay dos puertos HTTP?**
   El puerto 80 maneja la API y el 81 mantiene el stream MJPEG separado para no mezclar las peticiones de control con el flujo continuo de video.

4. **¿Que pasa si se cae Internet?**
   La deteccion local sigue funcionando. El evento se guarda en una cola RAM y se intenta enviar a Supabase cuando vuelve la conectividad.

5. **¿Que pasa si falla el correo?**
   El registro en Supabase no se pierde. El firmware diferencia entre evento registrado y correo enviado y reintenta SMTP.

6. **¿Por que no se guarda la contrasena normal de Gmail?**
   Se usa una contraseña de aplicacion. La credencial se configura localmente en el firmware y no debe compartirse.

7. **¿Como funciona 22:00 -> 06:00?**
   La franja se guarda en el dia de inicio. El algoritmo tambien comprueba la madrugada del dia siguiente.

8. **¿Que protege RLS?**
   RLS limita las filas que puede leer o modificar el cliente. En esta entrega las politicas estan restringidas al `DEVICE_ID` de la practica.

9. **¿Por que la app tiene reconexion?**
   Porque el ESP32 puede apagarse, perder Wi-Fi o reiniciarse. La app debe reflejar esa perdida y volver a consultar sin reiniciarse.

10. **¿Expo Go mantiene un Foreground Service real?**
    No se debe afirmar eso. Expo Go tiene limitaciones para codigo nativo persistente. La deteccion critica esta deliberadamente fuera de la app, en el ESP32. Un Foreground Service Android literal requeriria un development build y componente nativo.

## Flujo de demostracion

```text
Encender ESP32
  -> conectar telefono
  -> abrir Expo Go
  -> verificar video
  -> guardar horario
  -> activar Modo Fuera de casa
  -> provocar movimiento
  -> ver INTRUSION
  -> comprobar Supabase
  -> comprobar correo
  -> cortar Internet
  -> repetir movimiento
  -> recuperar Internet
  -> comprobar reintento
```

## Frase corta para explicar la robustez

> El sistema separa la deteccion critica de la interfaz movil: el ESP32 detecta y conserva eventos localmente, Supabase persiste la informacion y Expo Go supervisa y controla el sistema.
