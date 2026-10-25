# KytyPS5

[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20Linux%20x64-0078D4.svg)](#requisitos-do-sistema)
[![Status](https://img.shields.io/badge/status-yotei--bringup-brightgreen.svg)](#guia-de-execução-do-jogo)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

> [!IMPORTANT]
> **Branch `yotei-bringup`:** Esta branch concentra as correções, otimizações e diagnósticos específicos para a execução de **Ghost of Yōtei (PPSA26344)** no PC (testado com processadores AMD Ryzen e GPUs NVIDIA GeForce RTX 3070 / 4090). Para detalhes aprofundados dos switches internos, consulte também [docs/yotei-fork.md](docs/yotei-fork.md).

---

## 🎮 Guia de Execução do Jogo (Ghost of Yōtei - PPSA26344)

### 1. Visão Geral da Estrutura

| Caminho | Descrição |
| --- | --- |
| `F:\KytyPS5-Windows-x64\` | Diretório principal de execução do emulador no Windows. |
| `F:\KytyPS5-Windows-x64\kyty_emulator.exe` | Executável compilado do emulador com suporte a 64 MiB de stack na GPU thread e patches de bringup. |
| `F:\GOI\PPSA26344-app\` | Diretório contendo os arquivos extraídos (dump legítimo) do jogo Ghost of Yōtei. |
| `F:\KytyPS5-Windows-x64\_PipelineCache\` | Cache persistente de shaders compilados (`PPSA26344.bin` e journal `PPSA26344.v9.shaders`). |
| `F:\KytyPS5-Windows-x64\run_yotei.bat` | Script principal para inicialização configurada do jogo. |
| `F:\KytyPS5-Windows-x64\precompile_yotei_shaders.bat` | Script para pré-compilar a biblioteca de shaders offline antes de iniciar a gameplay. |

---

### 2. Passo a Passo para Executar

#### Passo A: Pré-compilar os Shaders (Opcional, mas Altamente Recomendado)
Evita congelamentos e travamentos (*stutters*) durante as cutscenes e na abertura de mundo:
```cmd
cd /d "F:\KytyPS5-Windows-x64"
precompile_yotei_shaders.bat
```
Esse comando lê o arquivo `_PipelineCache\PPSA26344.v9.shaders` e aquece o cache Vulkan da GPU (`PPSA26344.bin`).

#### Passo B: Iniciar o Jogo
Execute diretamente o script configurado:
```cmd
cd /d "F:\KytyPS5-Windows-x64"
run_yotei.bat
```

---

## 🔍 O Que é Cada Parte da Configuração de Execução

O comando de execução em `run_yotei.bat` combina variáveis de ambiente do runtime e parâmetros de linha de comando. Abaixo está o detalhamento completo do que cada flag faz:

### 1. Variáveis de Ambiente (`set KYTY_*`)

* **`KYTY_COMPUTE_CHAIN=1`**  
  Permite que dispatches de computação consecutivos rodem em cadeia contínua na GPU sem emitir barreiras redundantes no host Vulkan, reduzindo drasticamente o overhead de CPU.
* **`KYTY_READBACK_QUEUE=0`**  
  Desativa a fila genérica de readback lento, garantindo que leituras críticas da memória de vídeo não sofram latência cumulativa.
* **`KYTY_READBACK_TARGETED_PUBLICATION=1`**  
  Publica leituras de buffers específicos de forma direcionada, eliminando esperas globais e stalls desnecessários na GPU.
* **`KYTY_GPU_PALETTE_SANITIZE=0`**  
  Desativa a sanitização agressiva da GPU sobre buffers de ossos, garantindo a integridade anatômica da malha 3D da protagonista (Atsu) sem deformações de vértices.
* **`KYTY_BINDLESS_KEY_BOUNDS=1`**  
  Restringe a carga de tabelas de texturas bindless estritamente aos descritores alcançáveis pelos shaders em execução. Previne crashes de `VK_ERROR_DEVICE_LOST` no portão Torii e nas transições de cena.
* **`KYTY_BINDLESS_EAGER=1`**  
  Carrega tabelas de texturas de compute raros de forma imediata (eager) em vez de atrasar por 1 frame, evitando artefatos pretos.
* **`KYTY_BINDLESS_TSHARP_CHECK=1`**  
  Valida os registradores de descritores de textura `T#` antes de submeter ao pipeline Vulkan, substituindo entradas corrompidas por fallbacks seguros em vez de causar encerramento inesperado.
* **`KYTY_AVPLAYER_RECYCLE=1`**  
  Recicla e reutiliza buffers de vídeo decodificados (`AvPlayer`) imediatamente, prevenindo vazamentos de memória e travamentos na reprodução de vídeos/cinemáticas.
* **`KYTY_PRESENT_WHEN_READY=1`**  
  Apresenta o quadro renderizado ao display assim que a GPU termina a fila de comandos, eliminando quedas artificiais de taxa de quadros (*frame drops*).

---

### 2. Argumentos de Linha de Comando (`kyty_emulator.exe`)

```cmd
kyty_emulator.exe ^
  --game "F:\GOI\PPSA26344-app" ^
  --screen-width 1920 --screen-height 1080 ^
  --game-preset-priority Performance ^
  --gpu-frames-ahead 0 ^
  --gpu-timestamp-scale 200 ^
  --label-flush-interval-us 2000 ^
  --async-pipelines false ^
  --defer-dispatches false ^
  --relaxed-readback false ^
  --speculative-draws true ^
  --record-thread true ^
  --amd-cpu
```

* **`--game "F:\GOI\PPSA26344-app"`**  
  Indica o diretório raiz do aplicativo do jogo extraído no host.
* **`--screen-width 1920 --screen-height 1080`**  
  Define a resolução da janela e do render target de saída para 1080p (Full HD), aliviando o uso de memória VRAM em placas de 8 GB.
* **`--game-preset-priority Performance`**  
  Prioriza os perfis internos do jogo voltados a desempenho e taxa de quadros fluida.
* **`--gpu-frames-ahead 0`**  
  Trava a antecipação de quadros na fila do processador gráfico da CPU, sincronizando com semáforos atômicos. Evita sobrescrita de anel de memória (*ring-buffer clobbering*) e atrasos de entrada.
* **`--gpu-timestamp-scale 200`**  
  Ajusta a escala de tempo virtual reportada para a engine do jogo. Faz o Dynamic Resolution Scaling (DRS) entender que a GPU precisa de margem livre, mantendo a resolução adaptativa e a estabilidade visual.
* **`--label-flush-interval-us 2000`**  
  Controla a frequência (em microssegundos) de sincronização e flush dos labels de execução de GPU.
* **`--async-pipelines false`**  
  Força compilação determinística e síncrona dos pipelines gráficos essenciais, garantindo que nenhum efeito ou shader apareça incompleto durante a gameplay.
* **`--defer-dispatches false`**  
  Processa comandos de computação no momento exato em que são submetidos pela CPU guest.
* **`--relaxed-readback false`**  
  Garante coerência estrita de sincronização entre escritas da GPU e leituras da CPU guest.
* **`--speculative-draws true`**  
  Alimenta antecipadamente o escalonador de chamadas de desenho (*draw calls*) para manter as filas da GPU cheias e evitar ociosidade.
* **`--record-thread true`**  
  Separa o parsing de pacotes PM4 em uma thread secundária dedicada, liberando a thread principal da GPU.
* **`--amd-cpu`**  
  Aplica otimizações e instruções compatíveis com arquiteturas AMD Ryzen (Zen 2 / Zen 3 / Zen 4), ajustando flags de escalonamento e tratamento de instruções x86-64.

---

### 3. Estrutura de Diretórios de Runtime

* **`_PipelineCache/`**: Onde os shaders compilados para a GPU são armazenados.
  - `PPSA26344.bin`: Cache binário Vulkan proprietário já compilado pelo driver da placa de vídeo.
  - `PPSA26344.v9.shaders`: Registro (journal) de instruções de shaders extraídas diretamente do jogo.
* **`_SaveData/`**: Armazena os arquivos de savegame e configurações de progresso do jogador.
* **`_TempData/`**: Armazenamento volátil de dumps e alocações dinâmicas temporárias.

---

## 🛠️ Como Compilar o Emulador a partir do Código-Fonte

Caso você faça alterações no código em C++, siga o procedimento abaixo para gerar o binário atualizado:

1. **Abrir o Terminal com as Ferramentas de Build (Clang-CL + MinGW/Ninja)**:
   ```powershell
   cmd.exe /c "set PATH=C:\winlibs\mingw64\bin;%PATH% && ninja -C C:\Users\Anderson\KytyPS5\_Build\windows-custom kyty_emulator"
   ```
2. **Copiar o Novo Executável para a Pasta de Jogo**:
   ```powershell
   Copy-Item "C:\Users\Anderson\KytyPS5\_Build\windows-custom\kyty_emulator.exe" "F:\KytyPS5-Windows-x64\kyty_emulator.exe" -Force
   ```
3. **Executar o Jogo**:
   ```cmd
   F:\KytyPS5-Windows-x64\run_yotei.bat
   ```

---

## 📌 Requisitos de Sistema Recomendados
* **Processador**: AMD Ryzen 5 5600 / Intel Core i5-12400 ou superior.
* **Memória RAM**: 16 GB ou 32 GB DDR4/DDR5.
* **Placa de Vídeo**: NVIDIA GeForce RTX 3070 (8 GB) / RTX 4070 ou superior com drivers Vulkan 1.3 atualizados.
* **Sistema Operacional**: Windows 11 x64 (Build 22H2 ou superior).
