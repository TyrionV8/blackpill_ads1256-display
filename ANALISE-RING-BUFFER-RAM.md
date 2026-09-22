# Handoff técnico: ring buffer em RAM para aquisição ADS1256

## Status e escopo

Este documento registra a análise do firmware `blackpill_ads1256+display`, o
diagnóstico da aquisição observada na Raspberry Pi e o plano recomendado para
outro agente implementar um ring buffer exclusivamente na SRAM do STM32F411.

Nenhuma linha do firmware foi alterada durante o diagnóstico. Este documento é
um handoff; não contém a implementação.

Objetivo principal:

> impedir que atrasos temporários do USB CDC interrompam a aquisição do ADS1256.

Fora do escopo desta versão:

- flash NOR externa de 8 MiB;
- SPI1 para armazenamento;
- persistência após reset ou queda de energia;
- DMA da flash;
- sistema de arquivos;
- retenção durante desconexão USB prolongada.

## Resumo do diagnóstico

O firmware atual executa aquisição e transmissão no mesmo laço:

```text
ler 4 canais -> snprintf -> esperar CDC -> incrementar SEQ -> repetir
```

Quando o CDC retorna `USBD_BUSY`, a função de transmissão espera em um laço por
até um segundo. Durante essa espera não é iniciado um novo frame do ADS1256.

O relatório declarou “sem perda” porque `SEQ` só é incrementado depois dessa
espera. Se o laço parar por 300 ms, o próximo frame continua sendo apenas
`SEQ + 1`. Portanto, sequência contígua prova que nenhum frame já numerado sumiu
no CDC; não prova que todas as posições temporais esperadas foram adquiridas.

O padrão medido foi de rajadas próximas a 1 kHz separadas por pausas longas. Um
ring buffer em RAM, combinado com CDC não bloqueante, desacopla essas duas
atividades e deve absorver as pausas observadas.

## Evidências medidas

### Captura de 100 segundos informada pelo usuário

| Métrica | Resultado |
|---|---:|
| Frames válidos | 47.676 |
| Taxa pelo PC | 476,77 Hz |
| Taxa pelo `TIME_US` | 477,29 Hz |
| Saltos de `SEQ` | 0 |
| Mediana entre frames na MCU | 2,095 ms |
| Maior intervalo na MCU | 383,453 ms |

### Captura silenciosa na Raspberry, 10 segundos

| Métrica | Resultado |
|---|---:|
| Frames | 5.355 |
| Taxa pela MCU | 539,80 Hz |
| Mediana | 0,999 ms |
| p95 | 1,305 ms |
| p99 | 1,313 ms |
| Pausas acima de 100 ms | 29 |
| Maior pausa | 155,854 ms |

### Captura silenciosa gravada em RAM na Raspberry, 10 segundos

| Métrica | Resultado |
|---|---:|
| Frames | 3.428 |
| Taxa pela MCU | 345,49 Hz |
| Mediana | 0,999 ms |
| p95 | 1,306 ms |
| p99 | 1,313 ms |
| Pausas acima de 10 ms | 24 |
| Maior pausa | 376,597 ms |

A mediana de aproximadamente 1 ms demonstra que a cadência normal está próxima
de 1 kHz. As médias baixas são causadas por bloqueios episódicos.

### Captura anterior no repositório

[`captura_serial.csv`](captura_serial.csv) contém aproximadamente 100.005 frames
em 100 segundos usando este firmware, ou cerca de 1.000 frames/s. Isso confirma
que a taxa nominal já foi atingida em outro caminho de host.

## Problema de alimentação observado

A Raspberry apresentou:

```text
throttled=0x50000
```

O kernel registrou repetidamente:

```text
Undervoltage detected!
Voltage normalised
```

Também ocorreu reinicialização durante o diagnóstico. A alimentação da Raspberry
deve ser corrigida antes da validação final. O ring buffer absorve backpressure
temporário, mas não resolve reinicialização, desconexão longa ou alimentação
instável.

Após corrigir fonte/cabo, conferir antes e depois do teste:

```bash
vcgencmd get_throttled
```

O resultado deve permanecer `throttled=0x0`.

## Pontos relevantes do firmware atual

### Laço principal

Em [`Core/Src/main.c`](Core/Src/main.c), o fluxo atual é:

1. obter timestamp;
2. chamar `AdcADS1256_ReadDifferentialChannelFrame()`;
3. formatar uma linha em `csvLineText[128]`;
4. chamar `UsbCdcSerial_WriteBytesBlocking()`;
5. incrementar `sampleSequenceNumber`.

O timestamp é capturado antes da leitura dos quatro canais. Manter essa semântica
ou alterá-la conscientemente e documentar.

### Aquisição ADS1256

[`Core/Src/adc_ads1256.c`](Core/Src/adc_ads1256.c) faz polling de `DRDY` e usa
SPI2 bloqueante para a sequência de MUX/SYNC/WAKEUP/RDATA.

Para a primeira implementação do ring buffer, esse código pode permanecer
inalterado. O polling do ADC já produz a cadência próxima de 1 kHz. O bloqueio
problemático é o CDC depois da aquisição.

EXTI de `DRDY` e DMA de SPI2 podem ser avaliados depois, mas não devem ser
introduzidos junto com o primeiro ring buffer, pois aumentariam muito a área de
mudança e dificultariam isolar regressões.

### CDC bloqueante

[`Core/Src/usb_cdc_serial.c`](Core/Src/usb_cdc_serial.c) repete
`CDC_Transmit_FS()` enquanto o resultado for `USBD_BUSY`, com timeout de um
segundo.

[`USB_DEVICE/App/usbd_cdc_if.c`](USB_DEVICE/App/usbd_cdc_if.c) já possui
`CDC_TransmitCplt_FS()`, mas o callback não informa conclusão a uma fila.

Esse é o ponto que precisa mudar arquiteturalmente.

### Memória disponível

O STM32F411CEU6 possui 128 KiB de SRAM. A compilação atual usa aproximadamente:

```text
data = 332 bytes
bss  = 9.240 bytes
```

Há folga suficiente para um ring buffer de 2.048 frames com 24 bytes por frame,
desde que o agente confirme o novo uso total no arquivo `.map` e preserve margem
para stack, heap, USB e buffers de transmissão.

### Inconsistência do CubeMX

[`Core/Src/spi.c`](Core/Src/spi.c) configura SPI2 com prescaler 32, resultando em
aproximadamente 1,5 MHz com APB1 a 48 MHz.

O arquivo `blackpill_ads1256+display.ioc`, porém, declara prescaler 2 e calcula
24 Mbit/s. Não regenerar o projeto pelo CubeMX sem reconciliar essa divergência,
pois isso pode alterar o clock do ADS1256 e introduzir uma regressão independente.

## Arquitetura mínima recomendada

```text
                   produtor
ADS1256 polling -> AcquisitionFrame -> ring SPSC em RAM
                                            |
                                            v
                                consumidor CDC não bloqueante
                                            |
                                            v
                                      Raspberry Pi
```

Fluxo desejado no `main`:

```text
1. adquirir um frame do ADS1256
2. atribuir SEQ e TIME_US
3. inserir o frame binário na fila RAM
4. chamar UsbFrameTransmitter_Process(), que nunca espera
5. repetir imediatamente
```

Interrupções do USB continuam concluindo transferências em paralelo enquanto o
`main` espera `DRDY` ou lê o ADC.

## Estrutura binária do frame

Usar estrutura fixa, sem armazenar CSV no ring:

```c
typedef struct
{
    uint32_t sequence_number;
    uint32_t timestamp_us;
    int32_t channel[4];
} AcquisitionFrame;
```

Tamanho esperado: 24 bytes.

```c
_Static_assert(sizeof(AcquisitionFrame) == 24U,
               "AcquisitionFrame precisa ter 24 bytes");
```

O `SEQ` deve ser incrementado no momento da aquisição, antes de tentar inserir o
frame na fila e independentemente do estado do USB. Assim, um overflow produz
um salto detectável no host, além do contador explícito.

## Dimensionamento do ring buffer

Recomendação inicial:

- capacidade: 2.048 frames;
- memória de dados: `2.048 * 24 = 49.152 bytes` = 48 KiB;
- cobertura aproximada: 2,048 segundos a 1.000 frames/s;
- índice por máscara: `index & (capacity - 1)`;
- alocação estática;
- sem `malloc`.

Esse tamanho cobre com margem a maior pausa observada de 376,597 ms e até o
timeout antigo de um segundo. Depois da mudança, o timeout bloqueante deve ser
removido, mas a capacidade continua útil contra jitter do host.

Se 48 KiB comprometerem a margem de SRAM após os demais buffers, usar 1.024
frames:

- 24 KiB;
- aproximadamente 1,024 segundo de cobertura.

Não usar 4.096 frames sem conferir cuidadosamente o mapa de memória: seriam
96 KiB apenas para os frames.

## API sugerida para o ring buffer

```c
typedef struct
{
    AcquisitionFrame frames[ACQUISITION_RING_CAPACITY];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t high_watermark;
    volatile uint32_t overruns;
} AcquisitionRingBuffer;

void AcquisitionRingBuffer_Initialize(AcquisitionRingBuffer *ring);

bool AcquisitionRingBuffer_TryPush(
    AcquisitionRingBuffer *ring,
    const AcquisitionFrame *frame);

bool AcquisitionRingBuffer_TryPeek(
    const AcquisitionRingBuffer *ring,
    AcquisitionFrame *frame);

bool AcquisitionRingBuffer_TryPop(AcquisitionRingBuffer *ring);

uint32_t AcquisitionRingBuffer_Count(
    const AcquisitionRingBuffer *ring);
```

Não é obrigatório copiar exatamente essa API, mas as operações devem ser
não bloqueantes e ter resultado explícito.

### SPSC e concorrência

O desenho recomendado é produtor único/consumidor único:

- produtor: laço de aquisição no `main`;
- consumidor: máquina de estados do transmissor, também chamada pelo `main`;
- callback USB: apenas sinaliza conclusão de transferência.

Como produtor e consumidor podem inicialmente executar no mesmo contexto, a
implementação pode ser simples. Ainda assim, o callback USB altera a flag de
conclusão de forma assíncrona; usar `volatile` e barreira de memória adequada.

Se no futuro o produtor migrar para ISR de DRDY, manter:

- `head` escrito somente pelo produtor;
- `tail` escrito somente pelo consumidor;
- `__DMB()` antes de publicar um novo `head` e antes de reutilizar posição
  liberada.

## Política de overflow

Overflow não pode ser silencioso.

Política recomendada para a primeira versão:

1. incrementar `frames_acquired` e `SEQ` para todo frame concluído;
2. tentar inserir na fila;
3. se cheia, descartar o frame novo;
4. incrementar `ram_queue_overruns` e `frames_dropped_at_source`;
5. manter os frames antigos já enfileirados em ordem;
6. informar o contador ao host.

Como o `SEQ` já foi atribuído, o host verá uma lacuna quando a fila voltar a
escoar. O contador da MCU explica a causa dessa lacuna.

## CDC não bloqueante

Criar uma máquina de estados que nunca espere por `USBD_BUSY`.

Comportamento mínimo:

1. se existe transferência em voo, retornar imediatamente;
2. se a fila está vazia, retornar imediatamente;
3. copiar um ou mais frames para um buffer TX estático;
4. formatar CSV no buffer do consumidor, fora da aquisição;
5. chamar `CDC_Transmit_FS()` uma vez;
6. se retornar `USBD_BUSY`, preservar fila/buffer e tentar depois;
7. se retornar `USBD_OK`, marcar transferência em voo;
8. `CDC_TransmitCplt_FS()` marca conclusão;
9. somente após conclusão remover da fila os frames daquele bloco.

Nunca reutilizar ou modificar o buffer apontado ao USB enquanto a transferência
estiver em voo.

### Envio em lote

Transmitir uma linha por chamada deixa pouca margem perto de 1.000 frames/s.
Usar um buffer estático de 512 ou 1.024 bytes e agrupar várias linhas CSV.

Estratégia sugerida:

- até 8 frames por bloco;
- transmitir imediatamente quando houver 8 frames;
- se houver menos, transmitir após limite de latência de 4–5 ms;
- depois de uma pausa, continuar enviando lotes enquanto houver backlog;
- garantir que a taxa média de drenagem seja maior que a taxa do produtor.

O tamanho exato deve considerar o pior comprimento possível das linhas com
quatro `int32_t` negativos e `uint32_t` máximos. Usar `snprintf` com controle
rigoroso de limites.

Alternativa de maior desempenho: enviar frames binários e formatar CSV na
Raspberry. Isso reduziria CPU e bytes, mas altera o protocolo e não é requisito
da primeira versão.

## Máquina de estados sugerida para TX

```text
IDLE
  | fila possui dados
  v
BUILDING_BLOCK
  | bloco pronto
  v
SUBMITTING
  | USBD_BUSY --------------------> tentar novamente depois
  | USBD_OK
  v
IN_FLIGHT
  | CDC_TransmitCplt_FS
  v
COMPLETED -> remover N frames -> IDLE
```

Erros diferentes de `USBD_BUSY` devem incrementar contador e retornar ao estado
seguro sem remover frames indevidamente.

## Contadores obrigatórios

Manter ao menos:

- `frames_acquired`;
- `frames_enqueued`;
- `frames_dropped_at_source`;
- `ram_queue_overruns`;
- `ram_queue_high_watermark`;
- `usb_submit_busy_count`;
- `usb_submit_error_count`;
- `usb_blocks_completed`;
- `frames_transmitted`;
- maior duração/idade do backlog, se possível.

Emitir periodicamente diagnóstico como comentário, que o script Python já
ignora:

```text
# BUFFER,USED=120,HIGH=790,OVERRUN=0
# USB,BUSY=42,ERROR=0,SENT=100000
```

Não imprimir comentário a cada frame; usar intervalo de um ou mais segundos.

## Mudanças esperadas no repositório

Novos arquivos sugeridos:

```text
Core/Inc/acquisition_frame.h
Core/Inc/acquisition_ring_buffer.h
Core/Src/acquisition_ring_buffer.c
Core/Inc/usb_frame_transmitter.h
Core/Src/usb_frame_transmitter.c
```

Arquivos existentes provavelmente alterados:

- `Core/Src/main.c`: aquisição -> enqueue -> processamento TX;
- `Core/Src/usb_cdc_serial.c`: remover uso bloqueante do caminho principal ou
  substituir por API não bloqueante;
- `USB_DEVICE/App/usbd_cdc_if.c`: sinalizar conclusão TX;
- headers correspondentes;
- arquivos de build do CubeIDE, se não forem atualizados automaticamente;
- `grava_serial_csv.py`: interpretar métricas de buffer e diagnosticar lacunas
  temporais;
- `README.md`: documentar arquitetura, capacidade e limitações.

Não é necessário alterar inicialmente:

- sequência de aquisição do ADS1256;
- SPI2;
- configuração de `DRDY`;
- SPI3/display;
- SPI1;
- DMA;
- linker script.

## Plano de implementação por etapas

### Etapa 1 — ring buffer unitário

- implementar estrutura e operações;
- testar vazio, cheio, wrap de índices e overflow;
- usar capacidade pequena em teste para forçar wrap rapidamente;
- validar ordem FIFO e high-watermark.

### Etapa 2 — integrar aquisição sem mudar CDC

- montar `AcquisitionFrame` após cada leitura;
- atribuir `SEQ` antes do enqueue;
- confirmar que dados do ADC permanecem corretos;
- observar uso de SRAM no `.map`.

### Etapa 3 — transmissor CDC não bloqueante

- buffer TX estático;
- callback de conclusão;
- somente remover frames após conclusão;
- remover espera ativa por `USBD_BUSY` do caminho de aquisição.

### Etapa 4 — envio CSV em lote

- até 8 frames por bloco;
- limite de latência para lote parcial;
- drenagem mais rápida que 1.000 frames/s;
- telemetria de ocupação e erros.

### Etapa 5 — atualizar validador

- manter verificação de saltos de `SEQ`;
- detectar lacunas em `TIME_US`;
- ler contadores de overflow emitidos pela MCU;
- separar taxa de produção MCU da taxa de chegada no PC;
- reprovar captura se qualquer overflow for diferente de zero.

## Critérios de aceitação

### Funcionamento normal

- 100 segundos de captura;
- aproximadamente 1.000 frames/s;
- `ram_queue_overruns = 0`;
- `frames_dropped_at_source = 0`;
- nenhum salto de `SEQ`;
- nenhuma lacuna temporal acima da tolerância definida;
- fila volta a ocupação próxima de zero depois de jitter passageiro.

### Backpressure controlado

Simular host sem consumir por:

- 100 ms;
- 400 ms;
- 1 s;
- valor próximo à capacidade do ring.

Ao retomar:

- aquisição deve ter continuado;
- frames devem sair em ordem;
- backlog deve ser drenado;
- não deve haver perda enquanto a capacidade não for excedida;
- acima da capacidade, overflow e salto de `SEQ` devem ser explícitos.

### CDC

- forçar `USBD_BUSY` repetidamente;
- garantir que o `main` não faça espera ativa;
- validar que buffer TX não é sobrescrito em voo;
- testar desconexão e reconexão;
- confirmar que erros não removem frames ainda não transmitidos.

### Ring buffer

- wrap de `head`/`tail` perto de `UINT32_MAX`;
- capacidade cheia e vazia;
- milhares de voltas com capacidade reduzida;
- FIFO exato;
- high-watermark correto;
- política de descarte correta.

### Alimentação

- repetir testes somente com `vcgencmd get_throttled = 0x0`;
- confirmar ausência de novos eventos de subtensão no kernel.

## Limitações da solução somente em RAM

- Dados pendentes são perdidos se a Black Pill reiniciar ou perder energia.
- Uma desconexão maior que aproximadamente 2 segundos enche um ring de 2.048
  frames a 1 kHz.
- O ring absorve rajadas; não resolve consumidor permanentemente mais lento.
- Se a taxa média do CDC for menor que a aquisição, qualquer tamanho finito
  acabará enchendo.
- A conclusão do endpoint USB não prova que a Raspberry persistiu os dados no
  CSV; para isso seria necessário ACK do host, fora do escopo inicial.

## Armadilhas a evitar

- Não armazenar strings CSV de 128 bytes por posição; usar frame binário de
  24 bytes.
- Não chamar `snprintf` antes do enqueue.
- Não fazer espera ativa por `USBD_BUSY`.
- Não remover item antes do callback de conclusão TX.
- Não reutilizar o buffer TX durante transferência.
- Não incrementar `SEQ` depois da transmissão.
- Não esconder overflow.
- Não concluir “sem perda” apenas por sequência contígua.
- Não introduzir EXTI, DMA, SPI1 e ring buffer na mesma etapa.
- Não regenerar o CubeMX sem corrigir a divergência do SPI2.
- Não validar com a Raspberry em subtensão.

## Definição de pronto

A implementação estará pronta quando demonstrar, com alimentação estável, que:

1. a aquisição mantém cadência próxima de 1 kHz durante stalls temporários do
   CDC;
2. o `main` nunca espera por disponibilidade USB;
3. backlog é drenado em ordem após o stall;
4. todos os overflows são detectados e reportados;
5. o validador usa `SEQ`, `TIME_US` e contadores da MCU;
6. o uso de SRAM continua dentro dos 128 KiB com margem segura de stack.

## Referências

- [Datasheet STM32F411xC/xE — STMicroelectronics](https://www.st.com/resource/en/datasheet/stm32f411ce.pdf)
- [Datasheet ADS1256 — Texas Instruments](https://www.ti.com/lit/ds/symlink/ads1256.pdf)

