# sws
Simple Web Server

Servidor Web mínimo em C, desenvolvido para o TCC *Arquiteturas de
Concorrência em Servidores Web em C*. Um único núcleo HTTP (parsing de
requisições GET/HEAD e entrega de arquivos estáticos) é executado sob três
arquiteturas de concorrência, selecionadas pela linha de comando:

| Modelo   | Linux / macOS                                       | Windows                                   |
|----------|-----------------------------------------------------|-------------------------------------------|
| `fork`   | um processo por conexão (`fork`, `SIGCHLD` + `waitpid`) | um processo por conexão (`CreateProcess` + `WSADuplicateSocket`) |
| `thread` | uma thread por conexão (`pthread_create`, *detached*) | uma thread por conexão (`_beginthreadex`) |
| `event`  | uma thread, sockets `O_NONBLOCK`, `epoll` *edge-triggered* (macOS: `poll`) | uma thread, sockets não bloqueantes, `WSAPoll` |

Os experimentos do trabalho são feitos no Linux (Ubuntu 24.04). O suporte ao
Windows existe para testes funcionais: o Windows não tem `fork()` nem
`epoll`, então os modelos `fork` e `event` usam os equivalentes mais próximos
da plataforma, e os números medidos lá não são comparáveis aos do Linux.

## Compilação

Linux / macOS (requer `clang` ou `gcc` e `make`):

```bash
make
```

Para usar o gcc: `make CC=gcc`. Outros alvos: `make debug` (AddressSanitizer
e UBSan), `make format`, `make lint`, `make clean`.

Windows com MinGW-w64 / MSYS2 / w64devkit (o Makefile detecta o Windows e
liga com `ws2_32`):

```bash
make CC=gcc
```

Windows com Visual Studio (ou qualquer plataforma com CMake):

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Uso

```
sws -m <fork|thread|event> [-p porta] [-r diretório] [-b backlog] [-k segundos] [-v]
```

| Opção | Descrição | Padrão |
|-------|-----------|--------|
| `-m`, `--model` | arquitetura de concorrência (obrigatório) | — |
| `-p`, `--port` | porta TCP | `4545` |
| `-r`, `--root` | diretório dos arquivos estáticos | `./www` |
| `-b`, `--backlog` | tamanho da fila do `listen()` | `1024` |
| `-k`, `--keepalive` | tempo ocioso máximo de conexões persistentes; `0` fecha a conexão após cada resposta | `5` |
| `-v`, `--verbose` | registra cada requisição (não usar nos benchmarks) | desligado |
| `-h`, `--help` / `-V`, `--version` | ajuda / versão | |

Exemplo:

```bash
./sws --model event --port 8080
curl -i http://127.0.0.1:8080/
```

`Ctrl+C` (ou `SIGTERM`) encerra o servidor.

## Comportamento HTTP

- Métodos `GET` e `HEAD`; os demais recebem `405 Method Not Allowed`.
- HTTP/1.1 com conexões persistentes (*keep-alive*) e *pipelining*; HTTP/1.0
  fecha a conexão, a menos que o cliente envie `Connection: keep-alive`.
- O fluxo TCP é acumulado até a linha em branco (`\r\n\r\n`) que encerra os
  cabeçalhos; requisições fragmentadas ou concatenadas são tratadas.
- Respostas: `200`, `301` (diretório sem `/`), `400`, `403` (tentativa de
  sair do diretório raiz, ex. `/../`), `404`, `405`, `414`, `431` (cabeçalhos
  acima de 8 KiB), `500`, `505`.
- `/` e diretórios servem `index.html`; o `Content-Type` é escolhido pela
  extensão do arquivo.

## Organização do código

| Arquivo | Conteúdo |
|---------|----------|
| `src/main.c` | inicialização, socket de escuta e seleção do modelo |
| `src/cli.c` | interface de linha de comando |
| `src/http.c` | núcleo HTTP comum: parsing, resolução de caminho, montagem da resposta |
| `src/connection.c` | atendimento de uma conexão com I/O bloqueante (usado por `fork` e `thread`) |
| `src/model_fork.c` | multiprocessamento |
| `src/model_thread.c` | multithreading |
| `src/model_event.c` | laço de eventos (`epoll`/`poll`/`WSAPoll`) com máquina de estados por conexão |
| `src/net.c` | `socket`/`setsockopt(SO_REUSEADDR)`/`bind`/`listen`, envio e erros de `accept` |
| `src/platform.c` | diferenças entre POSIX e Winsock, sinais e limites do sistema |
| `src/log.c` | mensagens de log |

O núcleo HTTP é o mesmo nos três modelos, para que as diferenças medidas
venham apenas da arquitetura de concorrência.

## Testes funcionais

Com o servidor compilado, executa uma bateria de requisições contra os três
modelos (Linux, macOS, ou Windows via Git Bash/MSYS2):

```bash
scripts/smoke_test.sh
```

## Experimentos de desempenho (Linux)

Requisitos no Ubuntu 24.04:

```bash
sudo apt install build-essential clang wrk curl procps util-linux
```

Execução completa (3 modelos × 7 níveis de conexões × 3 repetições de 30 s):

```bash
scripts/benchmark.sh
```

Exemplo com servidor e cliente isolados em núcleos diferentes:

```bash
scripts/benchmark.sh -S 0-3 -C 4-7 -t 4 -c "100 1000 10000" -d 60 -r 5
```

Para cada modelo e número de conexões o script inicia um servidor novo, faz
um aquecimento, executa o `wrk` e coleta:

- **throughput**: requisições por segundo (`wrk`);
- **latência**: média, desvio padrão, percentis 50/90/99/99,9 e máxima (`wrk`);
- **CPU**: tempo de CPU do servidor (incluindo os processos filhos no modelo
  `fork`) dividido pelo tempo do teste, em % de um núcleo
  (`/proc/<pid>/stat`);
- **memória**: pico e média de RSS e de PSS do servidor e seus filhos. No
  modelo `fork` o RSS conta várias vezes as páginas compartilhadas por
  copy-on-write; o PSS divide essas páginas entre os processos e é a medida
  adequada para comparar os modelos;
- **threads/processos**: pico do número de threads e de processos.

Os resultados ficam em `results/<data>/`: `results.csv` (uma linha por
execução), `summary.csv` (média das repetições), `env.txt` (hardware, kernel e
parâmetros de rede usados) e `raw/` (saída completa do `wrk` e logs).
Opções: `scripts/benchmark.sh -h`.

Para 10 000 conexões simultâneas, verifique os limites do sistema (o servidor
eleva o limite de descritores abertos até o limite rígido do usuário):

```bash
ulimit -Hn
sysctl net.core.somaxconn net.ipv4.ip_local_port_range
```
