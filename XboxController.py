import os

import pygame
import socket
import time

UDP_IP   = "192.168.4.1"
UDP_PORT = 1234

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

pygame.init()
pygame.joystick.init()

if pygame.joystick.get_count() == 0:
    print("❌ Controle não detectado!")
    exit()

controle = pygame.joystick.Joystick(0)
controle.init()

print(f"✅ Controle: {controle.get_name()}")
print("🚗 F1 ESP32 iniciado!")
print("   Analógico esquerdo X → Direção diferencial")
print("   R2 → Todos para frente")
print("   L2 → Todos para trás")
print("   Botão A → DRS (asa móvel toggle)")
print("   Ctrl+C para sair\n")


def zona_morta(valor, zona=0.12):
    return valor if abs(valor) > zona else 0.0


def suavizar(valor, ultimo, fator=0.25):
    return ultimo + (valor - ultimo) * fator


ultimo_T = 0.0
ultimo_S = 0.0
ultimo_comando = ""

# DRS
BOTAO_DRS = 0
drs_estado = 0
botao_anterior = False

try:
    while True:

        pygame.event.pump()

        # Direção
        S = zona_morta(controle.get_axis(0))

        # Tração
        r2 = (controle.get_axis(5) + 1) / 2
        l2 = (controle.get_axis(4) + 1) / 2

        if r2 > 0.05:
            T = r2
        elif l2 > 0.05:
            T = -l2
        else:
            T = 0.0

        T = int(suavizar(T * 100, ultimo_T))
        S = int(suavizar(S * 100, ultimo_S))

        ultimo_T = T
        ultimo_S = S

        TRACAO = 1 if abs(T) > 5 else 0

        # Botão A
        botao_atual = controle.get_button(BOTAO_DRS)

        if botao_atual and not botao_anterior:
            drs_estado = 1 - drs_estado
            print(f"\n🔧 DRS {'ABERTO' if drs_estado else 'FECHADO'}")

        botao_anterior = botao_atual

        comando = f"T:{T} S:{S} TRACAO:{TRACAO} DRS:{drs_estado}"

        if comando != ultimo_comando:
            sock.sendto(comando.encode(), (UDP_IP, UDP_PORT))
            ultimo_comando = comando

            print(
                f"\r🎮 T:{T:+04d}  S:{S:+04d}  DRS:{drs_estado}",
                end=""
            )

        time.sleep(0.02)

except KeyboardInterrupt:
    print("\n\n👋 Encerrando...")
    pygame.quit()
    sock.close()
