"""
Actividad 4 - Control de iluminación por gestos (PC -> ESP32)
Microcontroladores - UMNG

Reconoce gestos de la mano con MediaPipe Gesture Recognizer (webcam) y envía
un carácter por USB-Serial al ESP32:

    Closed_Fist -> 'F'  (LED amarillo 30 %)
    Victory     -> 'V'  (LED azul 70 %)
    Open_Palm   -> 'P'  (LED rojo 100 %)
    Thumb_Down  -> 'D'  (Interrupción 1: secuencia Modo 1)
    Thumb_Up    -> 'U'  (Interrupción 2: secuencia Modo 2)

Uso:
    python gestos_esp32.py                  # COM5, cámara 0
    python gestos_esp32.py --port COM7      # otro puerto
    python gestos_esp32.py --sin-serial     # solo probar la cámara
Tecla 'q' o ESC para salir.
"""

import argparse
import os
import time
import urllib.request

import cv2
import mediapipe as mp
import serial
from mediapipe.tasks.python import BaseOptions, vision

MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/gesture_recognizer/"
             "gesture_recognizer/float16/latest/gesture_recognizer.task")
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gesture_recognizer.task")

# Gesto de MediaPipe -> (comando al ESP32, texto en pantalla)
GESTOS = {
    "Closed_Fist": ("F", "Puno -> Amarillo 30%"),
    "Victory":     ("V", "Victoria -> Azul 70%"),
    "Open_Palm":   ("P", "Palma -> Rojo 100%"),
    "Thumb_Down":  ("D", "Pulgar abajo -> Modo 1"),
    "Thumb_Up":    ("U", "Pulgar arriba -> Modo 2"),
}

SCORE_MIN = 0.60        # confianza mínima para aceptar un gesto
FRAMES_ESTABLES = 5     # frames seguidos con el mismo gesto antes de enviarlo

CONEXIONES = [(c.start, c.end) for c in vision.HandLandmarksConnections.HAND_CONNECTIONS]


def descargar_modelo():
    if not os.path.exists(MODEL_PATH):
        print("Descargando modelo gesture_recognizer.task ...")
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        print("Modelo descargado.")


def abrir_serial(puerto):
    """Abre el puerto sin activar DTR/RTS para que el ESP32 no se reinicie."""
    ser = serial.Serial()
    ser.port = puerto
    ser.baudrate = 115200
    ser.timeout = 0
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def dibujar_mano(frame, landmarks):
    h, w = frame.shape[:2]
    pts = [(int(lm.x * w), int(lm.y * h)) for lm in landmarks]
    for a, b in CONEXIONES:
        cv2.line(frame, pts[a], pts[b], (0, 255, 0), 2)
    for i, p in enumerate(pts):
        cv2.circle(frame, p, 4, (0, 0, 255), -1)
        cv2.putText(frame, str(i), (p[0] + 4, p[1] - 4), cv2.FONT_HERSHEY_PLAIN, 0.8, (255, 255, 255), 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--cam", type=int, default=0)
    ap.add_argument("--sin-serial", action="store_true", help="probar solo la cámara")
    args = ap.parse_args()

    descargar_modelo()

    ser = None
    if not args.sin_serial:
        try:
            ser = abrir_serial(args.port)
            print(f"Conectado al ESP32 en {args.port}")
        except serial.SerialException as e:
            print(f"No se pudo abrir {args.port}: {e}")
            print("Cierra el Serial Monitor de PlatformIO/Arduino y revisa el puerto.")
            return

    opciones = vision.GestureRecognizerOptions(
        base_options=BaseOptions(model_asset_path=MODEL_PATH),
        running_mode=vision.RunningMode.VIDEO,
        num_hands=2,
    )

    cap = cv2.VideoCapture(args.cam, cv2.CAP_DSHOW)
    if not cap.isOpened():
        cap = cv2.VideoCapture(args.cam)
    if not cap.isOpened():
        print("No se pudo abrir la cámara.")
        return

    candidato, conteo = None, 0
    ultimo_enviado = None
    texto_estado = "Esperando gesto..."
    respuesta_esp = ""
    t0 = time.monotonic()
    ultimo_ts = -1

    with vision.GestureRecognizer.create_from_options(opciones) as reconocedor:
        while True:
            ok, frame = cap.read()
            if not ok:
                continue
            frame = cv2.flip(frame, 1)  # efecto espejo
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            imagen = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

            ts = int((time.monotonic() - t0) * 1000)
            if ts <= ultimo_ts:
                ts = ultimo_ts + 1
            ultimo_ts = ts
            resultado = reconocedor.recognize_for_video(imagen, ts)

            # Mejor gesto válido entre las manos detectadas
            gesto, score = None, 0.0
            for i, categorias in enumerate(resultado.gestures):
                dibujar_mano(frame, resultado.hand_landmarks[i])
                if categorias:
                    c = categorias[0]
                    if c.category_name in GESTOS and c.score >= SCORE_MIN and c.score > score:
                        gesto, score = c.category_name, c.score

            # Anti-rebote: el gesto debe mantenerse varios frames
            if gesto is not None and gesto == candidato:
                conteo += 1
            else:
                candidato, conteo = gesto, 1 if gesto else 0

            if candidato and conteo == FRAMES_ESTABLES and candidato != ultimo_enviado:
                cmd, texto_estado = GESTOS[candidato]
                if ser:
                    ser.write(cmd.encode())
                print(f"[{time.strftime('%H:%M:%S')}] {candidato} ({score:.2f}) -> '{cmd}'")
                ultimo_enviado = candidato

            # Leer respuesta del ESP32
            if ser and ser.in_waiting:
                lineas = ser.read(ser.in_waiting).decode(errors="ignore").strip().splitlines()
                if lineas:
                    respuesta_esp = lineas[-1]

            # Interfaz
            cv2.rectangle(frame, (0, 0), (frame.shape[1], 95), (30, 30, 30), -1)
            det = f"{gesto} ({score:.2f})" if gesto else "Sin gesto valido (se mantiene el estado)"
            cv2.putText(frame, f"Detectado: {det}", (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
            cv2.putText(frame, f"Estado LEDs: {texto_estado}", (10, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
            cv2.putText(frame, f"ESP32: {respuesta_esp}", (10, 85), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 0), 1)
            cv2.imshow("Actividad 4 - Gestos ESP32 (q para salir)", frame)

            if (cv2.waitKey(1) & 0xFF) in (ord("q"), 27):
                break

    cap.release()
    cv2.destroyAllWindows()
    if ser:
        ser.close()


if __name__ == "__main__":
    main()
