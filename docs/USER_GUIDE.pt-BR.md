[English](USER_GUIDE.md) | Português (Brasil)

# Guia do usuário do Haumea Studio

O Haumea Studio é o editor completo para open.mp e SA-MP, construído sobre o GTA San Andreas.
Você coloca objetos, veículos, NPCs e todos os outros elementos do servidor no mundo do jogo,
desenha textdraws, verifica o mapa contra os limites do servidor e o testa no jogo, e o Haumea
salva o mapa e os textdraws em arquivos Pawn que o seu gamemode inclui.

As teclas abaixo são as padrão. Todas podem ser mudadas em **Preferências → Atalhos**, e
**Ajuda → Atalhos de teclado** (`F1`) lista todas.

![Haumea editando a Grove Street](images/editor.png)

## Configuração

O Haumea precisa de duas pastas:

- **Pasta do jogo**: a sua instalação do GTA San Andreas. O Haumea lê os arquivos do jogo e nunca os altera.
- **Workspace**: o servidor SA-MP ou open.mp para o qual você mapeia. O Haumea pega dele os mapas,
  os modelos personalizados, os arquivos de NPC e o servidor de teste.

Para instalar o Haumea, veja [Instalação](../README.pt-BR.md#instalação). O Haumea abre na **tela inicial**:

- **Workspaces** que você usou, do mais recente ao mais antigo, com a data em que foram abertos pela
  última vez. **Fixar** (a estrela) mantém um no topo; o botão de pasta o abre no gerenciador de
  arquivos, e × o tira da lista (a pasta continua lá). **Novo workspace** cria uma pasta de servidor
  com `gamemodes`, `filterscripts` e `scriptfiles`; **Abrir pasta como workspace** usa uma que você
  já tem.
- **Mapas** do workspace selecionado que você já abriu, com uma imagem de cada. Clique duas vezes
  num deles, ou selecione-o e pressione **Abrir mapa**. **Abrir workspace** o abre com um mapa novo.
- **GTA San Andreas**: a pasta do jogo, com a versão do cliente SA-MP encontrada lá e os modelos
  personalizados 0.3.DL do workspace. Uma nova pasta do jogo carrega depois de reiniciar.
- **Continuar sem workspace**, ou **Pular** para manter o que está aberto.

Na primeira vez, a pasta do jogo é pedida aqui antes de qualquer coisa carregar. Digite cada caminho
ou use **Procurar...** (no Linux isso precisa do `zenity` ou do `kdialog`). Marque **Abrir o último
workspace automaticamente** para pular a tela; **Arquivo → Trocar workspace** a mostra de novo. As
duas pastas também podem ser mudadas em **Arquivo → Workspace** ou **Preferências → Workspace**. O
título da janela mostra o workspace.

O Haumea guarda as próprias configurações, autosaves, mapas recentes, favoritos e prefabs na pasta
de configuração dele: `~/.config/haumea` no Linux, `%APPDATA%\Haumea` no Windows,
`~/Library/Application Support/Haumea` no macOS. Na primeira vez, ele copia para lá o
`<GTA>/haumea_data` de uma instalação antiga e deixa a pasta antiga como estava. Um Haumea dentro
da pasta do GTA, ou iniciado a partir dela, encontra a pasta do jogo sem perguntar. Para as opções
de linha de comando, veja [Building and testing](BUILDING.md#command-line) (em inglês).

Se arquivos necessários do GTA estiverem ausentes ou ilegíveis, a tela inicial lista os
arquivos e mantém a seleção de pasta aberta. Restaure os arquivos ou escolha outra instalação
completa para continuar.

## Mapa, TextDraws e Código

Escolha **Mapa**, **TextDraws** ou **Código** na barra superior para alternar entre os editores.
O mapa, o arquivo de textdraws e o documento de código continuam abertos ao alternar.
**Janelas** contém os painéis e utilitários do editor.

Os balões amarelos **Novo!** apresentam TextDraws e Código. Abrir um editor ou fechar o balão
remove o aviso dele, inclusive depois de reiniciar.

## Movimentação

Segure o botão direito do mouse na vista 3D para olhar em volta e, enquanto segura, voe com
`W`/`A`/`S`/`D` (`Shift` mais rápido, `Alt` mais devagar). O cursor some enquanto você olha e
volta para onde estava. Arrastar com o botão do meio desloca a vista e a roda dá zoom. O botão de
olhar pode ser mudado em **Preferências → Câmera**.

O quanto você enxerga é definido na janela **Exibir** (**Janelas → Exibir**): **Distância de
desenho do mundo** para as construções do jogo, **Distância de desenho do mapa** para os objetos do
seu mapa (eles aparecem até essa distância, seja qual for a distância de desenho do modelo), e
**Neblina** (Jogo é tão densa quanto no jogo, Leve só ao longe). **Destacar meu mapa** escurece as
construções do jogo. A visão do jogador (`P`) sempre usa as distâncias do jogo.

## Colocando objetos

1. Abra o **Navegador de objetos** (`Ctrl+1`) e escolha um objeto. Busque por nome, ID, uma lista
   ou um intervalo (`1337, 19000-19100`). Todo modelo, do jogo e do SA-MP, fica numa categoria
   conforme o que os próprios arquivos do jogo dizem dele (o nome, o arquivo IDE, o tamanho da
   colisão), e **Favoritos** guarda os que você usa com frequência.
2. Clique em **Colocar** e depois clique no mundo. `Esc` para de colocar.
3. Clique num objeto para selecioná-lo. Arraste o gizmo para movê-lo (`W`) ou girá-lo (`E`), ou
   digite valores exatos em **Informações do objeto** (`Ctrl+2`). `G` o põe no chão, `F` foca a
   câmera nele e `Ctrl+D` o duplica.

**Editar → Ir para coordenadas** (`Ctrl+J`) aponta a câmera para uma posição que você cola:
`x, y, z`, `x y z` com um ângulo depois, ou uma linha inteira `SetPlayerPos(playerid, x, y, z);`.
Ela só move a vista, então não há nada a desfazer. **Editar → Copiar posição da câmera** copia onde
a câmera está como `x, y, z`.

Clique com o botão direito num objeto no mundo ou no navegador e escolha **Encontrar instâncias**.
O Navegador de objetos lista as contagens por modelo e as posições. Informe vários IDs ou intervalos
e escolha **Ambos**, **Meu mapa** ou **Mundo do GTA**. **Ir para**, **Anterior** e **Próxima** movem a
câmera, inclusive entre interiores. Os resultados ficam amarelos e não alteram sua seleção de edição.
Fechar o navegador ou voltar aos modelos remove o destaque da busca.

**Semelhantes** compara dimensões ou proporções dos limites de colisão, com tolerância ajustável.
Proporções ignora o tamanho total; girar a base em 90 graus também permite encontrar correspondências.
Use as prévias para comparar o formato real. Modelos sem limites de colisão não entram na comparação.

A barra de ferramentas liga e desliga o encaixe por passos (o ímã), o encaixe em objetos (`N`) e o
seguir o chão. **Ferramentas** guarda as configurações deles, além do pincel para espalhar objetos.

Informações do objeto organiza tudo do mesmo jeito: o nome e a variável Pawn, as configurações
próprias do objeto, depois a posição. Mundo e streaming, anexo, materiais (texturas, cores e texto)
e grupo e nota vêm em seguida, fechados até você precisar deles. Selecione vários objetos para
mudar um campo em todos de uma vez.

## Elementos do SA-MP

A aba **SA-MP** do navegador coloca todo o resto de que um mapa de servidor precisa:

| Jogadores | Mundo |
| --- | --- |
| Atores, classes de jogador (pontos de spawn) | Pickups, 3D text labels, gang zones |
| NPCs e pontos de caminho de NPC (só open.mp) | Map icons, locais de som, checkpoints (só streamer) |

Os jogadores nunca veem 3D text labels, gang zones, map icons, locais de som e pontos de caminho
como objetos, então o editor mostra cada um como um pequeno ícone colorido (um T, um alto-falante,
um alfinete, um quadrado, um nó) que mantém o tamanho na tela; clique nele para selecioná-lo.

Cada linha mostra quantos o mapa tem; clique no número para selecionar todos. Os elementos são
salvos como chamadas nativas ou do streamer (`CreateActor`, `CreateDynamicPickup`, ...) e lidos de
volta de scripts existentes. Gang zones e map icons nativos são mostrados a um jogador pela função
`_ShowForPlayer(playerid)` do mapa.

Um **local de som** toca um audio stream (uma URL) ou um som do jogo (escolhido numa lista) para os
jogadores que chegam dentro do raio dele.

<img src="images/samp-elements.png" alt="A aba SA-MP" width="360">

## NPCs (só open.mp)

Os NPCs do open.mp são jogadores bots. Coloque um pela aba SA-MP e depois defina o nome, a skin, a
arma, a vida e o colete dele em Informações do objeto. Em **Movimento**, escolha o que ele faz:

- **Seguir um caminho**: coloque primeiro os pontos de caminho do NPC, clicando um depois do outro,
  e depois escolha o caminho. Salvo como `NPC_MoveByPath`.
- **Andar pelos nós**: anda pelos nós de caminho do jogo a partir de um ponto inicial
  (`NPC_PlayNode`). Edite os arquivos de nós em **Janelas → Nós** (`F3`); o Haumea os salva em
  `scriptfiles/NPCs/nodes` do servidor e nunca altera os arquivos do próprio jogo.
- **Tocar uma gravação**: toca um arquivo `.rec` de `npcmodes/recordings` (`NPC_StartPlayback`).

**Prévia do movimento** faz o NPC andar na viewport. Atores e NPCs também podem tocar uma animação
(biblioteca, nome, velocidade, repetição, travas, congelar e tempo), e você a vê na skin.

<img src="images/npc-object-info.png" alt="Informações do objeto de um NPC que segue um caminho" width="360">

## Veículos

Os veículos ficam no navegador como os objetos. Em Informações do objeto, defina a criação estática
(`AddStaticVehicleEx`) ou dinâmica, o tempo de respawn, as duas cores (-1 é aleatória), uma placa,
um paintjob e peças de tuning. Só aparecem as peças que servem no modelo, uma por slot.

**Trens** (Freight 537, Brown Streak 538, Tram 449) vão para os trilhos como o SA-MP os coloca:
colocar um o encaixa no trilho mais próximo e arrastá-lo o desliza pelo trilho (segure `Alt` para
movê-lo livremente), com os trilhos desenhados de leve enquanto um trem está selecionado ou sendo
colocado. Eles vêm de `data/Paths/tracks.dat` e `tracks3.dat` do seu jogo (o SA-MP não usa
`tracks2.dat` e `tracks4.dat`). O Freight puxa três vagões de carga e o Brown Streak três vagões de
passageiros, desenhados atrás dele como no jogo; o Tram anda sozinho. Só a locomotiva é escrita no
mapa, já que o jogo adiciona os vagões, e cada trem ocupa quatro ids de veículo. O ângulo dele só
diz para que lado ele anda: acima de 180 para um lado, até 180 para o outro. Trens precisam de
**Veículo estático**, porque o `CreateVehicle` do open.mp os recusa, e a Verificação do mapa avisa
isso, além de apontar um trem colocado a mais de 3 m de um trilho ou um vagão sozinho.

## Materiais

Em **Materiais**, nas Informações do objeto, cada slot de material de um objeto pode receber outra
textura, uma cor ou um texto. **Procurar texturas...** mostra as texturas para escolher, e
**Janelas → Navegador de texturas** tem todas as texturas, inclusive a lista de texturas do Texture
Studio. A vista 3D as mostra como o cliente SA-MP 0.3.7 mostra: o texto usa os arquivos de fonte
reais, incluindo as cores `{RRGGBB}`, e materiais recoloridos perdem o sombreamento como no jogo.

As fontes são procuradas pelo nome nesta ordem: a sua pasta de fontes, as fontes do sistema (e a
`windows/Fonts` do prefixo do Wine), a pasta do GTA e, por fim, as fontes que vêm com o Haumea. O
Haumea traz as fontes do próprio SA-MP (GTAWEAPON3 para os ícones de armas e caveiras, SampAux3) e
as coloca na sua pasta de fontes ao iniciar: `%APPDATA%\haumea\fonts` no Windows,
`~/.local/share/haumea/fonts` no Linux, `~/Library/Application Support/Haumea/fonts` no macOS.
**Abrir pasta de fontes** (ao lado da fonte em Informações do objeto, e em Preferências) a abre:
coloque qualquer `.ttf` lá e pressione **Procurar fontes de novo**. Sem Arial, Times New Roman ou
Courier New, a prévia usa Liberation Sans, Serif ou Mono, que têm as mesmas larguras de letra.
Informações do objeto mostra qual arquivo de fonte a prévia usa, e a Verificação do mapa avisa de
uma fonte que não está instalada.

## Modelos personalizados (open.mp e SA-MP 0.3.DL)

**Arquivo → Importar → Modelo personalizado... (0.3.DL)** coloca um DFF e um TXD na pasta `models` do workspace
e no `artconfig.txt`, para que jogadores do open.mp e do 0.3.DL os baixem. Os modelos
personalizados aparecem em **Modelos personalizados** no navegador e mantêm os IDs negativos no
mapa salvo.

## TextDraws

Os textdraws são salvos num arquivo próprio, separado do mapa. Escolha **TextDraws** na barra
superior: o mundo dá lugar à tela do jogo de 640x448, com uma cor lisa ou um quadro parado da vista
que você tinha. Escolha **Mapa** para voltar ao mundo; **Esc** também sai desse editor quando
nenhum textdraw está selecionado. Adicione textos, caixas, sprites e prévias de modelo, arraste-os na tela (o canto os
redimensiona) e defina o texto, a fonte, as cores e o estilo. Desfazer (Ctrl+Z) tem um histórico
próprio aqui.

**Novo**, **Abrir**, **Salvar** e **Salvar como** da janela trabalham no arquivo de textdraws (um
`.pwn` ou `.inc`) e seu XML (por exemplo, `hud.inc.xml`); Ctrl+S salva o par enquanto o modo está ligado.
O XML guarda camadas, imagens de referência, bloqueios, grupos, pranchetas, formas e guias da régua. Inclua o Pawn no seu gamemode e chame
`Haumea_<arquivo>_CreateTextDraws()` no `OnGameModeInit` e `Haumea_<arquivo>_ShowTextDraws(playerid)`
quando um jogador precisar vê-los. **Testar no jogo** inclui o arquivo de textdraws aberto como está.

Um mapa salvo por uma versão anterior ainda pode ter textdraws. Ao abri-lo, o Haumea oferece
**Mover textdraws para um arquivo separado** (`<mapa>_textdraws.pwn`); salve o mapa depois para
tirá-los dele. Se você os mantiver no mapa, eles aparecem na tela mas são somente leitura e são
salvos de volta sem mudanças.

O painel abre encaixado à direita, com seu próprio layout salvo. Arraste a aba para movê-lo
ou desencaixá-lo. **Tela** na barra superior reúne formato da tela, fundo, HUD, grade,
alinhamento, réguas e guias. **Comparar** abre **Prévias de tela**, uma janela que pode ser
movida, redimensionada ou encaixada. **…** na barra de arquivos contém **Salvar como**,
arquivos recentes e o servidor de destino do código. Use as abas de conteúdo, layout e
estilo para editar a seleção; **Opções de código** contém variável e escopo do Pawn, e
**Organizar** reúne agrupamento e alinhamento.

![O editor de TextDraws](images/textdraw-editor.png)

### Códigos de cor dos TextDraws

A tela segue as regras do jogo para os códigos no texto de um textdraw:

- `~r~` vermelho, `~g~` verde, `~b~` azul, `~y~` amarelo, `~p~` roxo, `~w~` e `~s~` cinza-claro,
  `~l~` preto (só minúsculas). Só a primeira letra conta: `~red~` é `~r~`.
- `~h~` deixa a cor atual 1,5 vez mais clara, e se acumula: `~h~~h~` é mais clara ainda.
- `~n~` começa uma nova linha. A nova linha começa na cor do próprio textdraw (TextDrawColor) mais
  o **último** código de cor antes dela, e nada mais. Linhas quebradas pelo tamanho do texto
  funcionam do mesmo jeito.
  - `~y~test~n~test`: as duas linhas são amarelas.
  - `test~n~test` (sem código): as duas linhas são TextDrawColor.
  - `~w~~h~test~n~test`: a primeira linha é branco brilhante, a segunda é TextDrawColor clareada
    uma vez (`~h~` foi o último código), não branca.
  - `~r~~h~test~n~test`: a segunda linha também é TextDrawColor clareada, não vermelho-claro.
  - `test~n~~w~test`: um código depois de `~n~` vale normalmente.
- `~k~~PED_FIREWEAPON~` mostra o nome de uma tecla no jogo; o Haumea o deixa de fora.

## Prefabs

Um prefab é um grupo de objetos salvo para colocar de novo. Selecione os objetos e use **Arquivo →
Exportar → Seleção como prefab** (`Ctrl+Shift+E`). Dê a ele um nome e tags e salve-o em **Mine**
(a sua pasta de configuração) ou no workspace.

A aba **Prefabs** do Navegador de objetos lista os seus, os do workspace e um conjunto **Starter**.
Busque por nome ou tag, escolha um e clique em **Colocar**; `Q`, `E` ou a roda do mouse o giram antes
de você clicar. **Como grupo** coloca os objetos dele num grupo com o nome do prefab.

## Ferramentas de mapa

**Ferramentas** também tem ferramentas de arranjo: **Matriz** (cópias em linha ou em círculo),
**Espelhar**, **Alinhar e distribuir**, **Pôr no chão e copiar como Pawn** (pôr no chão, copiar
como anexos ou deslocamentos) e **Ao longo de uma linha** (distribuir objetos por um caminho que
você clica). Anéis numerados marcam cada ponto; um alvo ciano e um segmento mostram onde
o próximo clique será colocado. Clique com o botão direito ou pressione Esc para terminar;
**Limpar** remove os pontos. Movimentos e giros exatos ficam no modo Delta das Informações do objeto.

**Pin de coordenadas**, em Ferramentas, serve como referência de alinhamento.
**Posicionar pin** adiciona outro pin; **Limpar pin** remove só o ativo. Arraste a cabeça
para mover o pin, o anel da base para medir ou a ponta para mover o destino. Ambos encaixam
nas superfícies e nos cantos, pontos médios das arestas, centros das faces e centro dos
limites de colisão dos objetos. Um losango azul indica o encaixe num ponto.

Segure **Shift** para uma reta em X/Y/Z, **Ctrl** para passos de ângulo horizontal (15° por
padrão), **Shift+Ctrl** para passos no plano horizontal ou **Alt** para soltar o encaixe.
Edite XYZ, copie coordenadas, use **Ponto médio** ou uma **Distância da superfície** exata.
Em **Ajustes da régua**, digite a distância, o ângulo horizontal e a inclinação.
**Régua até** vincula o destino a outro pin e acompanha seus movimentos. Arrastar essa
ponta a torna livre.

Em **Guias de alinhamento**, escolha as guias X/Y/Z, os eixos e a referência do objeto
(mínimo, centro, máximo ou origem). Com o encaixe em objetos (`N`) ligado, os objetos
encaixam nos pins e guias pelo centro ou pelas bordas. **Alinhar seleção ao pin** move a
seleção em grupo; **Alinhar seleção à régua** projeta a referência de cada objeto na linha,
preservando sua posição ao longo dela. **Distribuir entre as pontas** distribui os objetos
na régua: dois objetos vão para pontas opostas. Cada comando é um passo de desfazer.
**Girar na direção da régua** alinha o ângulo horizontal dos objetos com a linha.

Para uma pista, marque um pin em cada canto, vincule-os em **Régua até**, selecione os dois
objetos e alinhe-os à guia ou distribua entre as pontas. Deixe Z desmarcado para manter as
alturas. Pins e suas configurações de alinhamento ficam no XML do mapa, fora do código do jogo. Continuam visíveis ao fechar
Ferramentas ou trocar de painel; **Mostrar pins e guias** os oculta e desativa o encaixe
neles. Sair de Mapa os oculta até você voltar.

**Radar art a partir de objetos** (em Ferramentas) desenha os seus objetos no radar com gang zones.
Ela usa todos os objetos, a seleção ou um grupo, vistos de cima, e coloca as zonas no grupo
"radar art".

- **Cor**: uma cor, por grupo ou a partir das texturas dos objetos. Um contorno e partes altas mais
  claras são opcionais.
- **Detalhe**: o contador mostra quantas zonas são necessárias. **Ajustar a este número de zonas**
  define o detalhe para chegar a um número que você escolhe.
- **Mostrado como**: **zonas globais** (no máximo 1024 no servidor), **zonas por jogador** (1024
  para cada jogador) ou **zonas streamed por jogador** (criadas perto de cada jogador enquanto ele
  anda, para mapas grandes). As duas últimas são só do open.mp.

**Aplicar** cria as zonas, e um desfazer remove todas.

## Interiores

**Janelas → Interiores** lista os interiores do GTA. Clique num para ir até lá e mostrá-lo. Clique
com o botão direito para copiar o código Pawn que leva um jogador até lá (`SetPlayerInterior` e
`SetPlayerPos`).

## Verificando o mapa

- **Verificação do mapa** (`F6`) lista o que vai quebrar no jogo: limites, modelos desconhecidos,
  elementos fora do mundo, códigos de cor errados e mais. Clique num item para selecionar aquilo de
  que ele trata.
- **Visão do jogador** (`P`, ou a caixa em **Janelas → Exibir**) desenha o mapa como um jogador na
  câmera o veria: distâncias de desenho e de stream, os limites do streamer, o virtual world e o
  interior.

## Testar no jogo

**Testar no jogo** (`F5`) roda o mapa num servidor open.mp privado na pasta de configuração do
Haumea. O seu arquivo de mapa e o seu próprio servidor não são tocados. **Abrir o jogo** entra
nele, e **Recarregar mapa** pega as suas mudanças. Escolha o pacote do servidor e a porta em
**Preferências → Servidor de teste**. No Linux 64 bits, o servidor e o compilador do open.mp
precisam da biblioteca C de 32 bits: veja [Instalação](../README.pt-BR.md#instalação).

![Testar no jogo com o registro do servidor](images/testserver.png)

## Edição multiplayer

Várias pessoas podem editar um mapa ao mesmo tempo. **Arquivo → Multiplayer...** abre o painel:

- **Hospedar este mapa**: compartilha o mapa que está aberto. O painel mostra a porta (7790, a
  menos que você mude) e um **código da sessão** de seis letras. Com **Abrir a porta no meu
  roteador** (ligado por padrão) o Haumea pede ao seu roteador para redirecionar a porta (UPnP) e
  mostra o endereço pelo qual as pessoas entram pela internet.
- **Entrar**: digite o endereço, a porta e o código do anfitrião (ou a senha do servidor, abaixo).
  Entrar substitui o mapa e o arquivo de textdraws que você tem abertos pelos da sessão, então
  salve os seus antes.

Todos veem as mudanças dos outros na hora: objetos, veículos, todos os elementos do SA-MP e as
configurações deles, grupos e travas, materiais, anexos, prédios do GTA removidos ou movidos,
remoções por área, arte do radar, as textdraws do mapa, o arquivo do editor de TextDraws, pinos de
alinhamento, caminhos 3D, os nodes de NPC da ferramenta Nodes e modelos customizados do 0.3.DL (os arquivos DFF e TXD vão para quem não
os tem). Desfazer vale para os seus próprios passos; um passo em algo que outra pessoa mudou depois é
pulado, então nunca joga fora a mudança dela. A câmera de cada pessoa aparece como um ponto colorido
com o nome dela, e o que ela selecionou ganha uma caixa na cor dela; **Ir até** olha de onde ela
está olhando.

Cada pessoa salva a própria cópia com **Arquivo → Salvar**. Abrir ou começar outro mapa sai de
uma sessão em que você entrou; quando o anfitrião abre um, ele substitui o de todos. **Parar de
hospedar** ou **Sair** mantém o mapa como está. Cada pessoa salva os arquivos da ferramenta Nodes com o
**Salvar** dela.

Todos precisam da mesma versão do Haumea e do mesmo jogo. Na mesma rede, entre com o endereço
local do anfitrião (por exemplo `192.168.1.20`). Pela internet, o roteador precisa redirecionar a
porta: o Haumea pede isso a ele, e quando o seu roteador não faz UPnP, redirecione a porta à mão,
use uma VPN como o Tailscale ou rode o haumea-server (abaixo). Quando o seu computador tem um
endereço IPv6 público (comum mesmo atrás de CGNAT), o painel o mostra também: os outros entram por ele
quando o roteador deixa. A conexão é criptografada, e só entra quem tem o código.

### Uma sessão que fica no ar: haumea-server

O `haumea-server` mantém uma sessão numa máquina que fica sempre ligada, como a que roda o seu
servidor SA-MP ou open.mp, então ninguém precisa deixar o Haumea aberto para hospedar. Ele não
precisa do jogo nem de tela:

```sh
haumea-server --port 7790 --password "algo comprido" --state equipe.hmps
```

Abra essa porta TCP no firewall da máquina, e todos entram com o endereço dela, a porta e a senha.
A primeira pessoa a entrar numa sessão vazia envia o mapa que tem aberto, e ele vira o da sessão;
quem entra depois recebe o da sessão. Com `--state` a sessão, com os arquivos dos modelos
customizados, fica guardada nesse arquivo, cada mudança escrita na hora (em `equipe.hmps.log` até o
próximo salvamento), e volta quando o servidor reinicia, mesmo depois de o programa cair. Ele escuta
em IPv6 e IPv4 ao mesmo tempo. `haumea-server
--help` lista o resto (`--bind`, `--name`, `--max-clients`). Cinco senhas erradas de um mesmo
endereço o fazem esperar dez minutos.

Toda conexão é criptografada (X25519 e XChaCha20-Poly1305, do Monocypher), com chaves ligadas à
senha pelo Argon2: quem observa o tráfego não vê o mapa nem nada em que testar senhas. Alguém que
consiga se pôr no meio da conexão ainda poderia testar palpites contra uma entrada, então escolha
uma senha que ninguém adivinhe.

O Namaka também entra em sessões (`multiplayer_session`): vários agentes podem construir numa mesma
sessão, cada um no seu Haumea ou vários num só. Cada agente tem a própria sessão de edição; desfazê-
la ou usar desfazer só retira as edições daquele agente, e o que outra pessoa mudou por último fica
como ela deixou.

## Salvando

Em **Mapa**, o mapa é um arquivo Pawn. **Arquivo → Salvar** (`Ctrl+S`) o grava, e **Salvar como** escolhe um
arquivo novo ou o estilo do código:

- **Nativo**: `CreateObject`, `CreateActor`, ... para qualquer servidor.
- **Streamer**: `CreateDynamicObject`, ... com o plugin streamer, para mapas grandes.

Um mapa que usa o streamer continua em código streamer quando você o abre e salva. Construções do
GTA movidas ou excluídas viram chamadas `RemoveBuildingForPlayer`. As primeiras linhas do arquivo
salvo dizem quais funções chamar no seu gamemode. O código compila sem avisos tanto com os includes
do open.mp quanto com os do SA-MP 0.3.7.

- **Abrir**: **Arquivo → Abrir mapa** (`Ctrl+O`, ou solte um `.pwn` na janela) lê mapas `.pwn` e
  mapas `.db` do Texture Studio (de `scriptfiles/tstudio/SavedMaps`). Salvar um mapa do Texture
  Studio grava um `.pwn` novo e nunca altera o `.db`. O Texture Studio guarda as texturas por número
  na lista de texturas dele; quando o seu servidor tem o Texture Studio instalado, o Haumea usa a
  lista desse servidor.
- **Mesclar**: **Arquivo → Importar → Mesclar arquivo Pawn** adiciona outro `.pwn`, `.db`, `.map`
  do MTA:SA ou mapa IPL ao seu.
- **Pawn + XML**: salvar `mapa.pwn` também grava `mapa.pwn.xml`. O Pawn contém o código para
  o jogo; o XML em UTF-8 espelha a cena, com propriedades e materiais dos objetos, grupos, notas,
  objetos ocultos, camadas do radar, TextDraws incorporados, pins de alinhamento e vista da câmera.
  Mantenha os dois arquivos juntos ao copiar ou compartilhar um mapa editável. O jogo só precisa do Pawn.
- **Mapas antigos** continuam abrindo. O primeiro salvamento cria o XML e guarda uma cópia exata
  do Pawn anterior em `mapa.pwn.pre-xml.bak`. Só abrir o mapa não modifica seus arquivos.
- **Edição à mão**: quando o Pawn muda, ele fornece os valores do jogo; os metadados do XML são
  aplicados aos objetos correspondentes. Se o XML for inválido, o mapa abre pelo Pawn com um
  relatório; sobrescrever aquele XML fica bloqueado até repará-lo ou movê-lo.
  [Detalhes do formato](MAP_FORMAT.md) (em inglês).
- **Autosave**: enquanto um mapa tem mudanças não salvas, o Haumea salva cópias na pasta de
  configuração dele. **Arquivo → Recuperar salvamento automático** as abre.

## Editor de código

Escolha **Código** na barra superior (`Ctrl+9`) para editar Pawn com cores, números de linha,
desfazer/refazer e **Localizar / Substituir** (`Ctrl+F`). Abra um `.pwn` ou `.inc` existente com
**Abrir script...** ou comece pelo código gerado do mapa. **Nativo** e **Streamer** escolhem o
formato usado ao gerar código do mapa.

Enquanto o documento gerado não for editado, ele acompanha as mudanças do mapa. Quando você
edita o código, suas alterações ficam no documento. **Atualizar a partir do mapa** gera uma
cópia nova e pergunta antes de substituir alterações não salvas.

**Salvar script** (`Ctrl+S`) salva o documento num script separado; o nome padrão do código
gerado termina em `.script.pwn`. O editor protege o `.pwn` do mapa, que é gerado novamente ao
salvar o mapa visual. Editar o código não muda a cena 3D. Escolha **Windows-1252** ou **UTF-8**
para a codificação do script; use UTF-8 para caracteres que o Windows-1252 não representa.

O código não salvo fica como um rascunho recuperável na pasta de configuração do Haumea.
Ele reaparece ao abrir **Código** depois de reiniciar.

## Namaka (assistente de IA)

O Namaka, nome da lua interna de Haumea, deixa um assistente de IA (Claude Code, Claude Desktop ou
outro host MCP) ver e editar o mapa aberto por um socket local. Ele se chamava Agent Alpha.

**Janelas → Namaka** o instala. Ele precisa do Python 3.10 ou mais novo; o painel diz qual Python
encontrou e o que instalar se não houver nenhum.

- Com o **Python 3.14**, **Instalar** funciona offline com os arquivos que vêm com o Haumea.
- Com o 3.10-3.13, ele baixa só as poucas partes que o Haumea traz apenas para o 3.14.

A instalação roda em segundo plano: o painel mostra o progresso, **Cancelar** a interrompe e
**Abrir o registro** mostra o que aconteceu. O Namaka fica na pasta de configuração
(`~/.config/haumea/namaka` no Linux, `%APPDATA%\Haumea\namaka` no Windows), então atualizar o Haumea
não o move.

- **Reparar** o instala de novo; a instalação antiga fica até a nova funcionar.
- **Atualizar** aparece quando o Haumea vem com um Namaka mais novo, e o Haumea também o atualiza
  depois de uma atualização do editor.
- **Desinstalar** o remove.

**Copiar para o Claude Code** põe um comando `claude mcp add` na área de transferência, e **Copiar
para o Claude Desktop** uma entrada para o `claude_desktop_config.json`, os dois para esta
instalação.

Depois que o Namaka está instalado, o Haumea fica esperando por ele sempre que inicia: no Linux e no
macOS pelo `~/.cache/haumea-agent.sock`, que só você pode usar, e no Windows numa porta local com
uma chave nova a cada vez, gravada em `namaka\connection.json` na pasta de configuração. **Deixar o
Namaka se conectar** (**Preferências → Namaka**, ou o painel) desliga isso a partir do próximo
início. Só um Haumea por vez pode ser alcançado: se outro já está, o painel avisa. Iniciar o Haumea
com `--agent-socket` ainda usa esse socket no lugar.
O assistente normalmente olha o mapa sem mexer na sua vista. Quando você pede para ele mostrar algo
ou levar você a algum lugar, ele voa com a câmera até lá (`move_camera`) e avisa; `camera_back`
volta para onde você estava. Ele nunca mexe na câmera enquanto você arrasta o gizmo, coloca objetos
ou olha em volta com o botão direito do mouse.
Uma instalação do Agent Alpha continua funcionando; depois que o Namaka estiver configurado, aponte
o seu cliente MCP para ele e apague a pasta antiga. Mais detalhes no
[guia de configuração](../tools/release/AGENT_README.md) (em inglês).

## Atualizações

Quando sai uma versão nova, um aviso no canto superior direito avisa: **Novidades** mostra as notas
dela, **Atualizar agora** a baixa em segundo plano, e ela é instalada na próxima vez que o Haumea
iniciar (ou na hora com **Reiniciar agora**, depois que as suas mudanças forem salvas). O Haumea
confere o download com os checksums da versão e substitui só os próprios arquivos; os seus mapas,
configurações e qualquer outra coisa na pasta dele ficam como estão, e um arquivo do Haumea que
você alterou é guardado como `<arquivo>.local`. **Ajuda → Voltar para a versão anterior** restaura a versão
anterior. **Preferências → Atualizações** liga ou desliga a verificação e os downloads automáticos
e escolhe o canal; **Ajuda → Procurar atualizações** verifica na hora.

## Atividade no Discord

**Preferências → Privacidade → Mostrar atividade no Discord** compartilha o modo de edição
(**Mapa**, **TextDraws** ou **Código**) e o tempo da sessão enquanto o Discord para desktop está
aberto. A opção vem ligada e reconecta se o Discord for aberto depois. Desligá-la ou fechar o
Haumea limpa a atividade. Nomes de mapas, arquivos e caminhos de projetos nunca são enviados.
Não precisa de token de bot nem login no Discord pelo Haumea; as configurações de privacidade
de atividade do próprio Discord continuam valendo.

## Privacidade

O Haumea mantém o conteúdo dos seus mapas no seu computador. Os serviços de diagnóstico recebem
pings diários de uso e relatórios de travamento só com a sua permissão. A atividade no Discord
é controlada separadamente, como descrito acima.

O **ping diário de uso** só vai se você disser sim: a tela inicial pergunta uma vez (fechá-la sem
responder vale como não), e **Preferências → Privacidade** o liga ou desliga a qualquer momento.

Quando está ligado, o Haumea envia no máximo uma vez por dia, para `telemetry.itsneufox.xyz`:

- um ID de instalação aleatório (32 caracteres hexadecimais gerados no seu computador; ele não diz
  nada sobre você nem sobre o seu computador),
- a versão (por exemplo 1.0.0.57) e o canal (production ou beta, ou dev/pr para versões de teste),
- o sistema operacional: Linux, Windows ou macOS.

Isso só conta quantas instalações estão em uso, em quais versões e sistemas. É guardado até 12
meses depois do último envio da sua instalação. Desligar o ping apaga o ID de instalação; ligá-lo
de novo cria um novo.

**Relatórios de travamento e de erro**: quando o Haumea trava ou encontra um erro, ele guarda um
relatório, e no próximo início um aviso pergunta: **Ver relatório** mostra exatamente o que seria
enviado, e depois **Enviar**, **Não enviar** ou **Sempre enviar**. **Preferências → Privacidade**
define isso como Perguntar (o padrão), Sempre enviar ou Nunca enviar. Um relatório tem:

- a versão, o commit, o canal, o sistema operacional e a versão dele, o tipo de processador e os
  nomes da placa de vídeo e do driver,
- onde aconteceu no código do Haumea (a pilha, como deslocamentos nos próprios arquivos dele) e a
  mensagem de erro, se houver,
- as últimas linhas do registro do editor e qual modo e qual janela estavam em uso.

Antes de ser guardado, a sua pasta pessoal, o seu nome de usuário, a pasta do jogo e o workspace são
trocados por marcadores, e os nomes dos mapas ficam de fora; o conteúdo dos mapas nunca é incluído.
Um relatório só leva o ID de instalação se o ping diário também estiver ligado. Os relatórios são
enviados para `reports.itsneufox.xyz` e guardados por 6 meses; os não enviados são apagados depois
de 30 dias. Tipos novos de travamento são resumidos num canal privado do Discord do desenvolvedor (a
versão, o sistema, a placa de vídeo e onde no código aconteceu; nunca linhas do registro nem IDs de
instalação).

O servidor não guarda endereços IP nem registros de acesso, e só o desenvolvedor vê o que ele
recebe. [O que é enviado e guardado](https://telemetry.itsneufox.xyz/privacy) tem os detalhes (em
inglês).

## Atalhos

**Ajuda → Atalhos de teclado** (`F1`) lista todos os comandos com a tecla de cada um, e
**Preferências → Atalhos** os redefine.

## Painéis

Os painéis se encaixam em volta da vista 3D. Você também pode deixá-los flutuando, e eles se
encaixam nas bordas da tela e uns nos outros. **Janelas → Redefinir disposição dos painéis** os põe
de volta no lugar.
