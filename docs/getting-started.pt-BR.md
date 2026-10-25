**Português** · [English](getting-started.md)

# Primeiros passos

São três níveis, e a maioria dos projetos só precisa do primeiro:

1. **Nada** — um jogo feito para gamepad já funciona.
2. **Controles que chegam e saem** — para uma tela de escolha de controle ou UI por controle.
3. **Multiplayer local** — um jogador por controle.

## Nível 1: nada

Botões, sticks, gatilhos, touchpad, vibração e movimento chegam pelo próprio sistema de input da Unreal,
como as mesmas teclas que um controle de Xbox produz. Instalado o plugin, um projeto já feito para gamepad
aceita DualShock 4, DualSense, Joy-Cons e Pro Controllers — Input Mapping Contexts,
`Play Force Feedback Effect` e todo o resto continuam funcionando sem mudança.

O único bind feito à mão é o do que não existe no Xbox: movimento e os botões extras listados em
[Input](input.md). Mesmo aí, as teclas são da *engine*, não do plugin.

## Nível 2: controles que chegam e saem

Coloque um node **Wait For Controller Changes** e trate o pino `On Connected`. Ele dispara na hora para
cada controle que já está conectado, e depois para cada um que chegar. O porquê de ser um node único em vez
de um bind está em [Reacting to controllers](controllers.md).

## Nível 3: multiplayer local

1. **Desligue o *Skip Assigning Gamepad to Player 1*,** em *Project Settings → Maps & Modes → Local
   Multiplayer*. Ele vem ligado e, assim que existe um segundo jogador, empurra o input de cada gamepad
   para o jogador seguinte. O porquê de ser tão difícil de perceber está em
   [Troubleshooting](troubleshooting.md#every-controller-drives-the-wrong-player).
2. **Configure o GameMode:** o `DefaultPawnClass` e um `PlayerStart` na fase para cada jogador.
3. **Crie os jogadores** a partir do `BeginPlay`:

   `BeginPlay → Wait For Controller Changes → (On Connected) → Ensure Local Player For Controller → Restart Player`

   O **Ensure Local Player For Controller** reaproveita o player 0 para o primeiro controle, cria só os
   Local Players que faltam e devolve o Player Controller. O **Restart Player** escolhe um `PlayerStart`,
   faz o spawn do Pawn padrão e o possui. Um jogo com regras próprias de spawn pode usar **Spawn Actor** +
   **Possess** no lugar.
4. **Adicione o Mapping Context** no `BeginPlay` do seu PlayerController:

   `Self → Get Enhanced Input Local Player Subsystem → Add Mapping Context`

5. **Mais de quatro jogadores:** por padrão a Unreal para em quatro. Aumente o limite com **JSL4U Set Max
   Local Players** no `BeginPlay`, ou com o `MaxSplitscreenPlayers` da própria Unreal no
   `Config/DefaultEngine.ini`; os dois mudam o mesmo valor. Veja
   [More than four players](players.md#more-than-four-players).

### Bom saber

- **`bWasCreated` é false para o player 0,** porque a Unreal cria esse jogador antes do `BeginPlay`. Isso
  não quer dizer que o player 0 já tem o seu Pawn.
- **O Ensure Local Player For Controller pode ser chamado de novo.** Proteja o que vem depois dele: cheque
  se o Player Controller devolvido já possui um Pawn, ou mantenha um mapa `ConnectionId → Object` para
  atores e UI por controle.
- **No Event Possessed de um Pawn, use o `New Controller`,** ligado direto no **JSL4U Get Controllers
  Assigned To Player**. O **Get Player Controller 0** sempre aponta para o player 0, seja qual for o
  jogador dono do Pawn.

Testar dois jogadores no viewport de Play do editor pede um node a mais — veja
[Testing in the editor](troubleshooting.md#testing-in-the-editor).
