"""Reset do TX (ESP32-S3, USB-Serial-JTAG) sem efeitos escondidos.

Logica DTR/RTS do USB-Serial-JTAG (confirmada empiricamente nesta sessao): o estado DTR=0/RTS=1
reinicia o chip. Por isso:
  - a porta e aberta com as linhas configuradas ANTES de open() (pyserial aplica-as no open);
  - TX fica em repouso em DTR=0/RTS=0; um reset = RTS 0->1 (100 ms) ->0, DTR sempre 0
    (equivalente ao HardReset do esptool), nunca passa por DTR=1 (download mode);
  - RX e aberto como sempre foi lido nesta sessao (DTR=1/RTS=0), e nunca e tocado.
"""
import time
import serial

RESET_HOLD_S = 0.10


def open_tx(port):
    # Abrir com DTR=0/RTS=0 pre-configurados REINICIA o chip no macOS (probe openclose #1: o kernel
    # levanta as linhas no open e o pyserial baixa DTR depois -> passa por DTR=0/RTS=1). DTR=1/RTS=0
    # e o metodo ja comprovado seguro (peek/capturas); daqui desce-se para repouso em 0/0 sem passar
    # por 0/1, e o reset passa a ser so RTS.
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True
    s.rts = False
    s.open()
    s.dtr = False
    return s


def open_rx(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True
    s.rts = False
    s.open()
    return s


def pulse(s, clock=None):
    """Um reset limpo. Devolve (t_assert, t_release) em tempo do host.
    clock: funcao de relogio para os dois carimbos (por omissao time.time(), como sempre; ex.: time.monotonic_ns).
    t_assert e lido imediatamente antes do assert do RTS e t_release logo depois de o libertar. A sequencia
    eletrica nao depende do relogio escolhido."""
    clock = clock or time.time
    t0 = clock()
    s.rts = True
    time.sleep(RESET_HOLD_S)
    s.rts = False
    return t0, clock()


def reset_single(s, clock=None):
    return [pulse(s, clock)]


def enter_download(s):
    """Download mode (ROM, app NAO corre) - mesma sequencia que o esptool usa para USB-Serial-JTAG
    (USBJTAGSerialReset): DTR=1/RTS=0 100 ms -> 1/1 -> 0/1 (reset) 100 ms -> 0/0. Deixa a porta em
    repouso 0/0. Devolve a hora do host do reset."""
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.dtr = True
    s.rts = False
    time.sleep(0.1)
    s.rts = True
    s.dtr = False
    t = time.time()
    s.rts = True
    time.sleep(0.1)
    s.dtr = False
    s.rts = False
    return t


def release(s):
    """Sai do download mode com um reset limpo (DTR=0 garantido, so RTS) -> arranque normal."""
    s.dtr = False
    s.rts = False
    time.sleep(0.2)
    return pulse(s)


def reset_double(s, gap_s=0.30, clock=None):
    """Reset limpo + segundo reset deliberado gap_s depois de libertar o primeiro."""
    a = pulse(s, clock)
    time.sleep(gap_s)
    b = pulse(s, clock)
    return [a, b]
