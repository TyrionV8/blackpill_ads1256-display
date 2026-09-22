#!/usr/bin/env python3

import csv
import math
import time
import serial
from datetime import datetime

PORTA = "/dev/ttyACM0"
BAUDRATE = 115200
DURACAO_SEGUNDOS = 100
ARQUIVO_SAIDA = "captura_serial.csv"

# Deve bater com Core/Inc/adc_ads1256.h:
# ADC_ADS1256_SELECTED_CYCLING_READS_PER_SECOND_CENTI / 100.
TAXA_LEITURAS_ESPERADA_HZ = 4000#4374.00
CANAIS_POR_FRAME = 4
TAXA_FRAMES_ESPERADA_HZ = TAXA_LEITURAS_ESPERADA_HZ / CANAIS_POR_FRAME
TOLERANCIA_TAXA_PERCENTUAL = 5.0

COLUNAS = [
    "tempo_pc_iso",
    "tempo_pc_s",
    "FRAME",
    "SEQ",
    "TIME_US",
    "D1",
    "D2",
    "D3",
    "D4",
    "bytes_usb_linha",
]


def calcular_taxa(qtd_amostras, duracao_s):
    if qtd_amostras < 2 or duracao_s <= 0:
        return None
    return (qtd_amostras - 1) / duracao_s


def formatar_taxa(taxa_hz):
    if taxa_hz is None:
        return "indisponivel"
    erro_percentual = ((taxa_hz - TAXA_FRAMES_ESPERADA_HZ) /
                       TAXA_FRAMES_ESPERADA_HZ) * 100.0
    status = "OK" if abs(erro_percentual) <= TOLERANCIA_TAXA_PERCENTUAL else "FORA"
    return f"{taxa_hz:.2f} Hz ({erro_percentual:+.2f}% vs esperado, {status})"


def imprimir_relatorio(estatisticas):
    frames = estatisticas["frames"]
    seqs = estatisticas["seqs"]
    tempos_pc = estatisticas["tempos_pc"]
    tempos_mcu_us = estatisticas["tempos_mcu_us"]
    perdas_seq = estatisticas["perdas_seq"]
    duplicados_seq = estatisticas["duplicados_seq"]
    fora_de_ordem_seq = estatisticas["fora_de_ordem_seq"]

    print("\n=== Relatorio da captura ===")
    print(f"Arquivo: {ARQUIVO_SAIDA}")
    print(f"Duracao configurada: {DURACAO_SEGUNDOS:.3f} s")
    print(f"Taxa esperada de leituras ADC: {TAXA_LEITURAS_ESPERADA_HZ:.2f} Hz")
    print(f"Canais por frame: {CANAIS_POR_FRAME}")
    print(f"Taxa esperada de frames: {TAXA_FRAMES_ESPERADA_HZ:.2f} Hz")
    print(f"Tolerancia usada: +/-{TOLERANCIA_TAXA_PERCENTUAL:.2f}%")
    print(f"Linhas validas gravadas: {estatisticas['linhas_validas']}")
    print(f"Linhas de comentario ignoradas: {estatisticas['linhas_comentario']}")
    print(f"Linhas invalidas ignoradas: {estatisticas['linhas_invalidas']}")
    print(f"Bytes USB em frames validos: {estatisticas['bytes_usb_validos']}")

    if not frames:
        print("Nenhum frame valido recebido; nao ha como validar perda ou taxa.")
        return

    seq_inicial = seqs[0]
    seq_final = seqs[-1]
    frames_esperados_por_seq = seq_final - seq_inicial + 1
    frames_perdidos_por_seq = max(0, frames_esperados_por_seq - len(set(seqs)))
    perda_percentual = (frames_perdidos_por_seq / frames_esperados_por_seq) * 100.0

    print("\n--- Integridade por SEQ ---")
    print(f"SEQ inicial: {seq_inicial}")
    print(f"SEQ final: {seq_final}")
    print(f"Frames esperados pela faixa de SEQ: {frames_esperados_por_seq}")
    print(f"Frames unicos recebidos: {len(set(seqs))}")
    print(f"Frames perdidos detectados por salto de SEQ: {frames_perdidos_por_seq}")
    print(f"Perda estimada por SEQ: {perda_percentual:.4f}%")
    print(f"SEQs duplicados: {duplicados_seq}")
    print(f"SEQs fora de ordem: {fora_de_ordem_seq}")

    if perdas_seq:
        print("Saltos de SEQ detectados:")
        for perda in perdas_seq[:20]:
            print("  "
                  f"{perda['seq_anterior']} -> {perda['seq_atual']} "
                  f"(faltaram {perda['faltantes']} frames, "
                  f"tempo_pc={perda['tempo_pc_s']:.6f}s, "
                  f"TIME_US={perda['tempo_mcu_us']}us)")
        if len(perdas_seq) > 20:
            print(f"  ... mais {len(perdas_seq) - 20} saltos omitidos")
    else:
        print("Nenhum salto de SEQ detectado.")

    print("\n--- Taxa de aquisicao ---")
    duracao_pc_s = tempos_pc[-1] - tempos_pc[0]
    taxa_pc_hz = calcular_taxa(len(frames), duracao_pc_s)
    print(f"Duracao pelos timestamps do PC: {duracao_pc_s:.6f} s")
    print(f"Taxa medida pelo PC: {formatar_taxa(taxa_pc_hz)}")
    if duracao_pc_s > 0:
        vazao_usb_pc = estatisticas["bytes_usb_validos"] / duracao_pc_s
        print(f"Vazao USB media pelos frames validos: {vazao_usb_pc:.2f} bytes/s")

    duracao_mcu_s = (tempos_mcu_us[-1] - tempos_mcu_us[0]) / 1000000.0
    taxa_mcu_hz = calcular_taxa(len(frames), duracao_mcu_s)
    print(f"Duracao pelo TIME_US do MCU: {duracao_mcu_s:.6f} s")
    print(f"Taxa medida pelo MCU: {formatar_taxa(taxa_mcu_hz)}")

    if taxa_pc_hz is not None and taxa_mcu_hz is not None:
        diferenca_pc_mcu = taxa_pc_hz - taxa_mcu_hz
        print(f"Diferenca PC - MCU: {diferenca_pc_mcu:+.2f} Hz")

    if len(frames) >= 2:
        intervalos_pc_ms = [
            (tempos_pc[i] - tempos_pc[i - 1]) * 1000.0
            for i in range(1, len(tempos_pc))
        ]
        intervalos_mcu_ms = [
            (tempos_mcu_us[i] - tempos_mcu_us[i - 1]) / 1000.0
            for i in range(1, len(tempos_mcu_us))
        ]
        intervalo_esperado_ms = 1000.0 / TAXA_FRAMES_ESPERADA_HZ
        print("\n--- Intervalos entre frames ---")
        print(f"Intervalo esperado: {intervalo_esperado_ms:.6f} ms")
        print("PC:  "
              f"min={min(intervalos_pc_ms):.6f} ms, "
              f"med={sum(intervalos_pc_ms) / len(intervalos_pc_ms):.6f} ms, "
              f"max={max(intervalos_pc_ms):.6f} ms")
        print("MCU: "
              f"min={min(intervalos_mcu_ms):.6f} ms, "
              f"med={sum(intervalos_mcu_ms) / len(intervalos_mcu_ms):.6f} ms, "
              f"max={max(intervalos_mcu_ms):.6f} ms")

    print("\n--- Diagnostico final ---")
    sem_perda = (
        frames_perdidos_por_seq == 0 and
        duplicados_seq == 0 and
        fora_de_ordem_seq == 0
    )
    taxa_referencia = taxa_mcu_hz if taxa_mcu_hz is not None else taxa_pc_hz
    taxa_ok = (
        taxa_referencia is not None and
        math.isclose(
            taxa_referencia,
            TAXA_FRAMES_ESPERADA_HZ,
            rel_tol=TOLERANCIA_TAXA_PERCENTUAL / 100.0,
        )
    )
    print(f"Perda de dados: {'NAO detectada' if sem_perda else 'DETECTADA'}")
    print(f"Taxa de aquisicao: {'OK' if taxa_ok else 'FORA DO ESPERADO'}")


def main():
    inicio = time.time()
    estatisticas = {
        "linhas_validas": 0,
        "linhas_comentario": 0,
        "linhas_invalidas": 0,
        "frames": [],
        "seqs": [],
        "tempos_pc": [],
        "tempos_mcu_us": [],
        "perdas_seq": [],
        "duplicados_seq": 0,
        "fora_de_ordem_seq": 0,
        "ultimo_seq": None,
        "bytes_usb_validos": 0,
    }

    with serial.Serial(PORTA, BAUDRATE, timeout=1) as ser, \
         open(ARQUIVO_SAIDA, "w", newline="", encoding="utf-8") as f:

        writer = csv.writer(f)
        writer.writerow(COLUNAS)

        print(f"Lendo {PORTA} a {BAUDRATE} por {DURACAO_SEGUNDOS} segundos...")
        print(f"Salvando em: {ARQUIVO_SAIDA}")
        print(f"Taxa esperada: {TAXA_FRAMES_ESPERADA_HZ:.2f} frames/s "
              f"({TAXA_LEITURAS_ESPERADA_HZ:.2f} leituras ADC/s)")

        while time.time() - inicio < DURACAO_SEGUNDOS:
            bruto = ser.readline()
            bytes_usb_linha = len(bruto)

            if not bruto:
                continue

            linha = bruto.decode("utf-8", errors="replace").strip()

            if not linha:
                continue

            if linha.startswith("#"):
                estatisticas["linhas_comentario"] += 1
                print(linha)
                continue

            agora = time.time()
            tempo_pc_iso = datetime.now().isoformat(timespec="milliseconds")
            tempo_pc_s = agora - inicio

            partes = linha.split(",")

            if len(partes) == 7 and partes[0] == "FRAME":
                try:
                    seq = int(partes[1])
                    tempo_mcu_us = int(partes[2])
                    dados = [int(valor) for valor in partes[3:7]]
                except ValueError:
                    estatisticas["linhas_invalidas"] += 1
                    print(f"Linha ignorada (valor nao numerico): {linha}")
                    continue

                writer.writerow([
                    tempo_pc_iso,
                    f"{tempo_pc_s:.6f}",
                    partes[0],
                    seq,
                    tempo_mcu_us,
                    dados[0],
                    dados[1],
                    dados[2],
                    dados[3],
                    bytes_usb_linha,
                ])

                ultimo_seq = estatisticas["ultimo_seq"]
                if ultimo_seq is not None:
                    delta_seq = seq - ultimo_seq
                    if delta_seq > 1:
                        estatisticas["perdas_seq"].append({
                            "seq_anterior": ultimo_seq,
                            "seq_atual": seq,
                            "faltantes": delta_seq - 1,
                            "tempo_pc_s": tempo_pc_s,
                            "tempo_mcu_us": tempo_mcu_us,
                        })
                        print(f"{linha}  <-- PERDA: faltaram {delta_seq - 1} frame(s)")
                    elif delta_seq == 0:
                        estatisticas["duplicados_seq"] += 1
                        print(f"{linha}  <-- SEQ DUPLICADO")
                    elif delta_seq < 0:
                        estatisticas["fora_de_ordem_seq"] += 1
                        print(f"{linha}  <-- SEQ FORA DE ORDEM")
                    else:
                        print(linha)
                else:
                    print(linha)

                estatisticas["linhas_validas"] += 1
                estatisticas["frames"].append(partes)
                estatisticas["seqs"].append(seq)
                estatisticas["tempos_pc"].append(tempo_pc_s)
                estatisticas["tempos_mcu_us"].append(tempo_mcu_us)
                estatisticas["ultimo_seq"] = seq
                estatisticas["bytes_usb_validos"] += bytes_usb_linha
            else:
                estatisticas["linhas_invalidas"] += 1
                print(f"Linha ignorada: {linha}")

    print("Finalizado.")
    imprimir_relatorio(estatisticas)


if __name__ == "__main__":
    main()
