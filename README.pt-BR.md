**Português** · [English](README.md)

# JoyShockLibrary4Unreal

Plugin para Unreal Engine que faz controles de PlayStation e Nintendo funcionarem nativamente no jogo:
DualShock 4, DualSense (inclusive o Edge), Joy-Con, Pro Controller e Pro Controller 2, com recursos que
só eles têm, como giroscópio, touchpad, light bar e junção de dois Joy-Cons num jogador só. O Pro
Controller 2 conecta por Bluetooth sem precisar parear no Windows.

**Ele preenche as lacunas, não substitui o input da Unreal.** Tudo o que um gamepad comum faz — botões,
sticks, gatilhos, vibração, movimento — chega ao jogo pelo próprio sistema de input da Unreal, como as
mesmas teclas e os mesmos nodes que um controle de Xbox já usa. Os nodes `JSL4U*` existem só para o que a
Unreal e o Windows não conhecem: calibração do giroscópio, a light bar, juntar dois Joy-Cons, escolher qual
controle é de qual jogador. Escreva o jogo com as APIs da engine e ele aceita qualquer gamepad; use um node
`JSL4U*` quando quiser algo que só esses controles fazem.

## Sumário

- [Controles suportados](#controles-suportados)
- [Instalação](#instalação)
- [Primeiros passos](#primeiros-passos)
- [Documentação](#documentação)
- [Perguntas](#perguntas)
- [Créditos](#créditos)

## Controles suportados

- **PlayStation:** DualShock 4, DualSense e DualSense Edge, por USB e Bluetooth.
- **Switch 1:** Joy-Con (sozinho na horizontal ou em par), por Bluetooth, e Pro Controller, por USB e
  Bluetooth.
- **Switch 2:** Pro Controller 2, por USB e Bluetooth. O Joy-Con 2 está implementado, só por Bluetooth,
  mas ainda não teve o funcionamento confirmado.
- **Xbox e outros controles XInput** continuam com a Unreal; o plugin convive com eles e respeita os
  lugares de jogador que eles ocupam.

O que já foi testado em cada um, e com quem pedir teste, está em
[docs/hardware.md](docs/hardware.md).

## Instalação

- Baixe ou clone este repositório na pasta `Plugins` do seu projeto. O caminho da pasta Content deve ficar
  assim: `<projeto>/Plugins/JoyShockLibrary4Unreal/Content`.
- Confira se o JoyShockLibrary4Unreal está ativado no `.uproject` ou em *Edit → Plugins*.
- No Windows, nenhum outro plugin é necessário: o hidapi já vem junto. Em outras plataformas o plugin usa o
  [HIDUE](https://github.com/microdee/HIDUE), do microdee, para falar HID; adicione-o ao projeto também.

## Primeiros passos

Há três níveis, e a maioria dos projetos só precisa do primeiro:

1. **Nada.** Instalado o plugin, um jogo feito para gamepad já aceita esses controles: Input Mapping
   Contexts, `Play Force Feedback Effect` e o resto continuam funcionando.
2. **Reagir a controles que chegam e saem**, para uma tela de escolha de controle ou UI por controle: o
   node **Wait For Controller Changes**.
3. **Multiplayer local:** **Ensure Local Player For Controller** para cada controle que conecta.

O passo a passo de cada nível está em [docs/getting-started.md](docs/getting-started.md).

O plugin traz uma fase de demonstração, `LV_JoyShockDemo`, com multiplayer local e um espelho 3D de cada
controle conectado. Veja [docs/demo.md](docs/demo.md).

## Documentação

As páginas detalhadas estão em inglês.

| Página | O que tem |
|---|---|
| [Hardware support](docs/hardware.md) | O que foi testado em cada controle, e quem pode testar o quê |
| [Getting started](docs/getting-started.md) | Os três níveis de configuração, mais de quatro jogadores e o PIE |
| [Demo level](docs/demo.md) | Como a fase de demonstração é montada |
| [Input](docs/input.md) | Botões exclusivos, touchpad, Joy-Con na horizontal, convenção dos sticks |
| [Motion and gyro](docs/motion.md) | Movimento pelo Enhanced Input, espaço do giroscópio, calibração |
| [Reacting to controllers](docs/controllers.md) | Os nodes **Wait For…**, o `ConnectionId` e a bateria |
| [Players and local multiplayer](docs/players.md) | Lugares de jogador, mais de quatro jogadores, junção de Joy-Cons |
| [Rumble, lights and output](docs/output.md) | Vibração, luzes, indicador de jogador, conflito com outros programas |
| [Nintendo Switch 2 controllers](docs/switch2.md) | Pro Controller 2 e Joy-Con 2, USB e Bluetooth, sensores extras |
| [Blueprint nodes](docs/blueprint-nodes.md) | Como a API `JSL4U*` está organizada |
| [Diagnostics and troubleshooting](docs/troubleshooting.md) | Comandos de diagnóstico e problemas conhecidos de Bluetooth |
| [Source layout](docs/architecture.md) | Para quem vai mexer no código: as camadas, onde mudar o quê |

## Perguntas

**Em que versões da Unreal foi testado?** Só na 5.8. O plugin usa a API `FInputDeviceRegistry`. A Epic
Games sinalizou o `FInputDeviceScope` como obsoleto, e ele será descontinuado em versões futuras da engine.

**Por que usar isto em vez do Steam Input ou do DS4Windows?** Eles traduzem esses controles para XInput, e
fazem isso bem, inclusive dando mira por giroscópio a jogos que não a têm. Com este plugin o jogo aceita os
controles e seus recursos por conta própria, sem o jogador precisar de programa rodando em segundo plano.

**Dá para usar o processamento de movimento com as bibliotecas oficiais da Sony e da Nintendo?** Nenhuma
biblioteca oficial foi usada no desenvolvimento, então não sabemos. As funções de `GamepadMotion.hpp`
processam dados de movimento venham de onde vierem.

## Créditos

- JibbSmart, por criar a [JoyShockLibrary](https://github.com/JibbSmart/JoyShockLibrary) original e por
  responder às perguntas enviadas por DM no Twitter. Os créditos completos da biblioteca original estão no
  repositório dele.
- microdee, pelo plugin [HIDUE](https://github.com/microdee/HIDUE), a camada HID do plugin fora do Windows.
- O modelo 3D do DualSense é de [Saleem Akhtar](https://www.artstation.com/marketplace/p/zBM9R/ps5-duelsense-controller-3d-model-fbx).
- Os modelos do Joy-Con, do grip do Joy-Con e do DualShock 4 usados no espelho da demo foram modelados por
  [uayten](https://github.com/uayten) para este plugin.
- [uayten](https://github.com/uayten), pela reforma de 07/2026: input nativo e multiplayer local na Unreal
  Engine 5.8; descoberta, identidade e roteamento estáveis de controles e jogadores; Joy-Con na horizontal,
  junção, separação e LEDs de jogador; a implementação completa do Pro Controller 2; bateria;
  troca entre cabo e Bluetooth; o espelho por controle, o HUD de sensores e o fluxo da demo; eixos de stick
  consistentes para a engine; e as correções de conexão, travamento e crash ao fechar.
