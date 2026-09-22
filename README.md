# Blackpill STM32F411 - Projeto modular legivel

Este pacote separa o codigo em modulos mais claros, com nomes mais longos e descritivos.
A intencao aqui e facilitar leitura humana, nao encurtar o codigo.

## Estrutura

Core/Inc/application_config.h
- Configuracoes gerais da aplicacao: endereco inicial da Flash, LED onboard e tamanho do buffer USB.

Core/Inc/application_error.h + Core/Src/application_error.c
- Tratamento de erro e pisca rapido do LED.
- Contem o Error_Handler(). Nao deixe outro Error_Handler() duplicado no main.c.

Core/Inc/adc_ads1256.h + Core/Src/adc_ads1256.c
- Driver simples do ADC ADS1256 usando SPI2.
- Prefixo usado: AdcADS1256_...

Core/Inc/display_st7735.h + Core/Src/display_st7735.c
- Driver simples do display TFT ST7735 usando SPI3.
- Prefixo usado: DisplayST7735_...

Core/Inc/microsecond_delay.h + Core/Src/microsecond_delay.c
- Delay em microssegundos usando DWT->CYCCNT.

Core/Inc/acquisition_frame.h
- Estrutura binaria fixa de 24 bytes com SEQ, TIME_US e quatro canais.

Core/Inc/acquisition_ring_buffer.h + Core/Src/acquisition_ring_buffer.c
- Ring buffer estatico com 4.096 frames (96 KiB, cerca de 4,096 s a 1 kHz).
- Quando cheio, faz `tail = head`, descarta todo o backlog e mantem somente o frame novo.

Core/Inc/usb_frame_transmitter.h + Core/Src/usb_frame_transmitter.c
- Consumidor CDC nao bloqueante, com lotes de ate oito frames e latencia maxima de 4 ms para lote parcial.
- So remove frames do ring depois do callback de conclusao da transferencia USB.

Core/Inc/usb_cdc_serial.h + Core/Src/usb_cdc_serial.c
- Escrita bloqueante usada somente pelas mensagens de inicializacao, antes da aquisicao.

Core/Src/main.c
- Fica apenas com a orquestracao da aplicacao:
  inicializa perifericos, display e ADS1256 e entra no loop de aquisicao para o ring buffer.

## Ring buffer de aquisicao

O caminho continuo de dados e:

```text
ADS1256 -> AcquisitionFrame -> ring buffer em SRAM -> formatacao CSV em lote -> USB CDC
```

Durante uma sessao ativa, o `SEQ` avanca para toda aquisicao concluida,
inclusive quando o ring esta cheio.
Antes de a Raspberry abrir a porta CDC e ativar o DTR, as leituras do ADS1256
sao ignoradas. Ao abrir a porta, fila, `SEQ` e contadores reiniciam; o primeiro
frame dessa sessao recebe `SEQ=0`.

Quando a fila cheia descarta o backlog, a lacuna aparece no host.
Uma vez por segundo o firmware tambem envia comentarios de diagnostico que
`grava_serial_csv.py` interpreta:

```text
# BUFFER,USED=120,HIGH=4096,OVERRUN=1,FLUSHES=1,DROPPED=4096,OVERWRITTEN=4096,ACQUIRED=100000,ENQUEUED=100000
# USB,BUSY=42,ERROR=0,BLOCKS=12500,SENT=100000
```

O buffer absorve atrasos temporarios do host, mas nao persiste dados depois de
reset e nao evita perda se a USB ficar indisponivel por mais que sua capacidade.
Com frames de 24 bytes, cinco segundos exigiriam aproximadamente 120.000 bytes
somente para a fila e nao deixariam margem segura nos 128 KiB da STM32F411.

## Como aplicar no STM32CubeIDE

1. Copie os arquivos .h para Core/Inc.
2. Copie os arquivos .c para Core/Src.
3. Substitua o seu Core/Src/main.c pelo main.c deste pacote.
4. Confirme que nao ficou outro Error_Handler() duplicado em outro arquivo gerado manualmente.
5. Faca Clean Project e depois Build Project.

## Observacoes importantes

- O ADS1256 esta usando hspi2.
- O display ST7735 esta usando hspi3.
- A aplicacao esta configurada para iniciar em 0x08004000 por causa do bootloader HID da WeAct.
- O linker script tambem precisa estar configurado para iniciar em 0x08004000.
