// The fork's own on-screen texts in the game's language (ui_text_switch.h).
#include "ui_text_switch.h"

#include <switch.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "../overlay/hostui.h"
#include "../runtime.h"

namespace ui_text {
namespace {

const Texts kEnglish = {
    "SwitchWakerHD: first start",
    "The graphics have not been prepared on this console yet.",
    "On this first start some textures may look black and some objects may appear a moment late: the console "
    "prepares each graphics effect the first time it is drawn. It gets better as you play, and what is prepared is "
    "kept for the next starts.",
    "Prepare graphics now",
    "Next, choose your Quest Log (or start a new game you do not save): the game then visits every place by itself "
    "behind a loading screen (about 30-40 minutes, best docked) and restarts at the title screen, ready. Your saves "
    "are not touched.",
    "Play now",
    "The console prepares the most common graphics first behind a loading screen (about 20 minutes; B there to play "
    "at once). Prepare graphics stays in the settings menu: hold Minus (-) in the game, Picture tab.",

    "SwitchWakerHD: new in this version",
    "Prepare graphics: the console can now prepare the graphics of the whole game at once, so that places you have "
    "not visited yet are not black or late the first time.",
    "The game visits every place by itself behind a loading screen (about 30-40 minutes, best docked), then "
    "restarts at the title screen, ready. Your saves are not touched. What is already prepared on this console goes "
    "faster.",
    "Later",
    "This message does not show again. Prepare graphics stays in the settings menu: hold Minus (-) in the game, "
    "Picture tab.",

    "SwitchWakerHD: the game's files were not found",
    "SwitchWakerHD needs your own copy of the game in %s/ (the folders code/, content/ and meta/), as "
    "tools/switch/make_sd.py lays it out from your dump (INSTALL.md).",
    "Missing: %s",
    "Copy the build/sd/ folder made by make_sd.py to the root of the SD card, then start the game again.",
    "Press + to close.",

    "Preparing graphics",
    "%zu of %zu",
    "starting",
    "About %d min left",
    "Working out the time left...",
    "The game is visiting every place by itself to prepare its graphics, with the sound and rumble off. It restarts "
    "at the title screen when it is done. Your saves are not touched.",
    "Stopping after this place... What is done is kept.",
    "Keep holding B to stop",
    "Hold B to stop. What is done is kept, and Prepare graphics continues from here next time.",

    "Prepare graphics (experimental)",
    "Stop preparing graphics",
    "What is prepared so far is kept; Prepare graphics continues from here next time.",
    "The console prepares each graphics effect the first time it is drawn, so new places can look black or show "
    "objects late for a few seconds. Prepare graphics does it for the whole game at once: the game visits every "
    "place by itself (30-40 minutes, best docked), then restarts at the title screen. Your saves are not touched.",
    "The battery is at %u%%: connect the charger or dock the console first.",
    "Continue preparing graphics (%zu of %zu done)",
    "Prepare graphics again",
    "Prepare graphics",
    "Done on this console. Again is only useful after an update that changed the graphics.",

    "Graphics ready: the whole game has been prepared on this console.",
    "Preparing graphics: %zu of %zu (%s)%s. To stop: hold B.",
    ", about %d min left",
    "Preparing graphics: %s. The game restarts.",
    "complete",
    "stopped (start it again to continue)",
    "a place did not load; start it again to continue",
    "Preparing graphics: done. Close the game and start it again (your saves are as they were).",
    "already running",
    "start a game first (your Quest Log, or a new game you do not save)",
    "could not copy the Quest Log files (is the SD card full?)",
    "Prepare graphics: choose your Quest Log (or start a new game you do not save) and it begins by itself.",
    "Preparing the graphics",
    "First start: the console prepares the graphics of the game once, so that places are not black or late the first time. This takes about 20 minutes, best docked.",
    "Starting the game...",
    "Reading the game's graphics...",
    "%zu of %zu",
    "B: play now (what is left goes on in the background)",
    "Removes the black shadows and missing effects that appear the first time a place is drawn, because the "
    "console has no shader cache yet, and prepares the Switch for much smoother play afterwards. It visits the "
    "whole game once by itself (30-40 minutes, best docked). Over 90% of the shaders can be prepared; a few "
    "cannot be compiled.",
    "Unfinished: place %zu of %zu. Continue below to carry on from there.",
    "Done on this console. Repeating it only helps after an update that changes the graphics.",
    "Not done yet on this console.",
};

const Texts kSpanish = {
    "SwitchWakerHD: primer arranque",
    "Los gráficos aún no se han preparado en esta consola.",
    "En este primer arranque algunas texturas pueden verse negras y algunos objetos pueden aparecer con un momento "
    "de retraso: la consola prepara cada efecto gráfico la primera vez que se dibuja. Mejora a medida que juegas, y "
    "lo preparado se guarda para los siguientes arranques.",
    "Preparar los gráficos ahora",
    "Después elige tu diario (o empieza una partida nueva que no guardes): el juego visita entonces cada lugar por "
    "sí solo detrás de una pantalla de carga (unos 30-40 minutos, mejor con la consola en la base) y vuelve a la "
    "pantalla de título, listo. Tus partidas no se tocan.",
    "Jugar ahora",
    "La consola prepara primero los gráficos más comunes detrás de una pantalla de carga (unos 20 minutos; B allí "
    "para jugar enseguida). Preparar gráficos sigue en los ajustes: mantén pulsado Menos (-) en el juego, pestaña "
    "Picture.",

    "SwitchWakerHD: novedad en esta versión",
    "Preparar gráficos: la consola ya puede preparar de una vez los gráficos de todo el juego, para que los lugares "
    "que aún no has visitado no se vean negros ni tarden la primera vez.",
    "El juego visita cada lugar por sí solo detrás de una pantalla de carga (unos 30-40 minutos, mejor con la "
    "consola en la base) y después vuelve a la pantalla de título, listo. Tus partidas no se tocan. Lo que esta "
    "consola ya tiene preparado va más rápido.",
    "Más tarde",
    "Este mensaje no volverá a aparecer. Preparar gráficos sigue en los ajustes: mantén pulsado Menos (-) en el "
    "juego, pestaña Picture.",

    "SwitchWakerHD: no se encuentran los archivos del juego",
    "SwitchWakerHD necesita tu propia copia del juego en %s/ (las carpetas code/, content/ y meta/), tal como la "
    "prepara tools/switch/make_sd.py a partir de tu volcado (INSTALL.md).",
    "Falta: %s",
    "Copia la carpeta build/sd/ que crea make_sd.py en la raíz de la tarjeta SD y vuelve a abrir el juego.",
    "Pulsa + para cerrar.",

    "Preparando los gráficos",
    "%zu de %zu",
    "empezando",
    "Quedan unos %d min",
    "Calculando el tiempo que queda...",
    "El juego está visitando cada lugar por sí solo para preparar sus gráficos, sin sonido ni vibración. Al "
    "terminar vuelve a la pantalla de título. Tus partidas no se tocan.",
    "Parando tras este lugar... Lo hecho se conserva.",
    "Mantén B para parar",
    "Mantén pulsado B para parar. Lo hecho se conserva y la próxima vez Preparar gráficos sigue desde aquí.",

    "Preparar gráficos (experimental)",
    "Parar la preparación",
    "Lo preparado hasta ahora se conserva; la próxima vez Preparar gráficos sigue desde aquí.",
    "La consola prepara cada efecto gráfico la primera vez que se dibuja, así que los lugares nuevos pueden verse "
    "negros o mostrar objetos con retraso durante unos segundos. Preparar gráficos lo hace para todo el juego de una "
    "vez: el juego visita cada lugar por sí solo (30-40 minutos, mejor con la consola en la base) y después vuelve a "
    "la pantalla de título. Tus partidas no se tocan.",
    "La batería está al %u%%: conecta el cargador o pon la consola en la base primero.",
    "Seguir preparando los gráficos (%zu de %zu hechos)",
    "Preparar los gráficos otra vez",
    "Preparar los gráficos",
    "Hecho en esta consola. Repetirlo solo sirve tras una actualización que cambie los gráficos.",

    "Gráficos listos: todo el juego está preparado en esta consola.",
    "Preparando los gráficos: %zu de %zu (%s)%s. Para parar: mantén B.",
    ", quedan unos %d min",
    "Preparando los gráficos: %s. El juego se reinicia.",
    "terminado",
    "parado (vuelve a lanzarlo para seguir)",
    "un lugar no cargó; vuelve a lanzarlo para seguir",
    "Preparación terminada. Cierra el juego y vuelve a abrirlo (tus partidas están como estaban).",
    "ya está en marcha",
    "empieza antes una partida (tu diario, o una partida nueva que no guardes)",
    "no se pudieron copiar los archivos del diario (¿está llena la tarjeta SD?)",
    "Preparar gráficos: elige tu diario (o empieza una partida nueva que no guardes) y empezará sola.",
    "Preparando los gráficos",
    "Primer arranque: la consola prepara una vez los gráficos del juego, para que los lugares no se vean negros ni tarden la primera vez. Tarda unos 20 minutos, mejor con la consola en la base.",
    "Arrancando el juego...",
    "Leyendo los gráficos del juego...",
    "%zu de %zu",
    "B: jugar ya (lo que falte sigue en segundo plano)",
    "Quita las sombras negras y los efectos que faltan la primera vez que se dibuja un lugar, porque la consola "
    "aún no tiene los shaders en caché, y hace que después el juego vaya mucho más fluido. Visita todo el juego "
    "una vez por sí solo (30-40 minutos, mejor con la consola en la base). Se puede preparar más del 90% de los "
    "shaders; unos pocos no se pueden compilar.",
    "Sin terminar: lugar %zu de %zu. Sigue abajo para continuar desde ahí.",
    "Hecho en esta consola. Repetirlo solo sirve después de una actualización que cambie los gráficos.",
    "Aún no se ha hecho en esta consola.",
};

const Texts kFrench = {
    "SwitchWakerHD : premier démarrage",
    "Les graphismes n'ont pas encore été préparés sur cette console.",
    "Lors de ce premier démarrage, certaines textures peuvent apparaître noires et certains objets s'afficher avec "
    "un léger retard : la console prépare chaque effet graphique la première fois qu'il est dessiné. Cela s'améliore "
    "en jouant, et ce qui est préparé est conservé pour les démarrages suivants.",
    "Préparer les graphismes maintenant",
    "Choisissez ensuite votre journal (ou une nouvelle partie non sauvegardée) : le jeu visite alors chaque lieu tout "
    "seul derrière un écran de chargement (environ 30-40 minutes, de préférence sur la station d'accueil), puis "
    "revient à l'écran titre, prêt. Vos sauvegardes ne sont pas touchées.",
    "Jouer maintenant",
    "La console prépare d'abord les graphismes les plus courants derrière un écran de chargement (environ 20 "
    "minutes ; B pour jouer tout de suite). Préparer les graphismes reste dans les réglages : maintenez Moins (-) en "
    "jeu, onglet Picture.",

    "SwitchWakerHD : nouveau dans cette version",
    "Préparer les graphismes : la console peut maintenant préparer en une fois les graphismes de tout le jeu, pour "
    "que les lieux pas encore visités ne soient ni noirs ni en retard la première fois.",
    "Le jeu visite chaque lieu tout seul derrière un écran de chargement (environ 30-40 minutes, de préférence sur la "
    "station d'accueil), puis revient à l'écran titre, prêt. Vos sauvegardes ne sont pas touchées. Ce qui est déjà "
    "préparé sur cette console va plus vite.",
    "Plus tard",
    "Ce message ne s'affichera plus. Préparer les graphismes reste dans les réglages : maintenez Moins (-) en jeu, "
    "onglet Picture.",

    "SwitchWakerHD : fichiers du jeu introuvables",
    "SwitchWakerHD a besoin de votre propre copie du jeu dans %s/ (les dossiers code/, content/ et meta/), telle que "
    "tools/switch/make_sd.py la prépare à partir de votre dump (INSTALL.md).",
    "Manquant : %s",
    "Copiez le dossier build/sd/ créé par make_sd.py à la racine de la carte SD, puis relancez le jeu.",
    "Appuyez sur + pour fermer.",

    "Préparation des graphismes",
    "%zu sur %zu",
    "démarrage",
    "Environ %d min restantes",
    "Calcul du temps restant...",
    "Le jeu visite chaque lieu tout seul pour préparer ses graphismes, sans son ni vibrations. Il revient à l'écran "
    "titre à la fin. Vos sauvegardes ne sont pas touchées.",
    "Arrêt après ce lieu... Ce qui est fait est conservé.",
    "Maintenez B pour arrêter",
    "Maintenez B pour arrêter. Ce qui est fait est conservé, et la préparation reprendra d'ici la prochaine fois.",

    "Préparer les graphismes (expérimental)",
    "Arrêter la préparation",
    "Ce qui est préparé est conservé ; la préparation reprendra d'ici la prochaine fois.",
    "La console prépare chaque effet graphique la première fois qu'il est dessiné : les nouveaux lieux peuvent donc "
    "paraître noirs ou afficher des objets en retard pendant quelques secondes. Préparer les graphismes le fait pour "
    "tout le jeu en une fois : le jeu visite chaque lieu tout seul (30-40 minutes, de préférence sur la station "
    "d'accueil), puis revient à l'écran titre. Vos sauvegardes ne sont pas touchées.",
    "La batterie est à %u%% : branchez le chargeur ou placez la console sur sa station d'abord.",
    "Continuer la préparation (%zu sur %zu faits)",
    "Préparer les graphismes à nouveau",
    "Préparer les graphismes",
    "Fait sur cette console. Recommencer n'est utile qu'après une mise à jour qui change les graphismes.",

    "Graphismes prêts : tout le jeu est préparé sur cette console.",
    "Préparation des graphismes : %zu sur %zu (%s)%s. Pour arrêter : maintenez B.",
    ", environ %d min restantes",
    "Préparation des graphismes : %s. Le jeu redémarre.",
    "terminée",
    "arrêtée (relancez-la pour continuer)",
    "un lieu ne s'est pas chargé ; relancez-la pour continuer",
    "Préparation terminée. Fermez le jeu et relancez-le (vos sauvegardes sont intactes).",
    "déjà en cours",
    "lancez d'abord une partie (votre journal, ou une nouvelle partie non sauvegardée)",
    "impossible de copier les fichiers du journal (la carte SD est-elle pleine ?)",
    "Préparer les graphismes : choisissez votre journal (ou une nouvelle partie non sauvegardée) et elle démarrera toute seule.",
    "Préparation des graphismes",
    "Premier démarrage : la console prépare une fois les graphismes du jeu, pour que les lieux ne soient ni noirs ni en retard la première fois. Environ 20 minutes, de préférence sur la station d'accueil.",
    "Démarrage du jeu...",
    "Lecture des graphismes du jeu...",
    "%zu sur %zu",
    "B : jouer maintenant (le reste continue en arrière-plan)",
    "Supprime les ombres noires et les effets manquants qui apparaissent la première fois qu'un lieu est dessiné, "
    "car la console n'a pas encore de shaders en cache, et rend le jeu bien plus fluide ensuite. Le jeu visite "
    "tout seul chaque lieu une fois (30-40 minutes, de préférence sur la station d'accueil). Plus de 90 % des "
    "shaders peuvent être préparés ; quelques-uns ne peuvent pas être compilés.",
    "Inachevé : lieu %zu sur %zu. Continuez ci-dessous pour reprendre à partir de là.",
    "Fait sur cette console. Recommencer n'est utile qu'après une mise à jour qui change les graphismes.",
    "Pas encore fait sur cette console.",
};

const Texts kGerman = {
    "SwitchWakerHD: erster Start",
    "Die Grafik wurde auf dieser Konsole noch nicht vorbereitet.",
    "Beim ersten Start können manche Texturen schwarz aussehen und manche Objekte etwas verzögert erscheinen: Die "
    "Konsole bereitet jeden Grafikeffekt vor, wenn er zum ersten Mal gezeichnet wird. Das wird beim Spielen besser, "
    "und Vorbereitetes bleibt für die nächsten Starts erhalten.",
    "Grafik jetzt vorbereiten",
    "Wähle danach deinen Spielstand (oder starte ein neues Spiel, das du nicht speicherst): Das Spiel besucht dann "
    "hinter einem Ladebildschirm jeden Ort von selbst (etwa 30-40 Minuten, am besten in der Station) und startet "
    "fertig im Titelbildschirm neu. Deine Spielstände werden nicht verändert.",
    "Jetzt spielen",
    "Die Konsole bereitet zuerst die häufigsten Grafiken hinter einem Ladebildschirm vor (etwa 20 Minuten; B dort, "
    "um sofort zu spielen). Grafik vorbereiten bleibt im Einstellungsmenü: Halte im Spiel Minus (-), Reiter Picture.",

    "SwitchWakerHD: neu in dieser Version",
    "Grafik vorbereiten: Die Konsole kann jetzt die Grafik des ganzen Spiels auf einmal vorbereiten, damit noch nicht "
    "besuchte Orte beim ersten Mal weder schwarz noch verzögert sind.",
    "Das Spiel besucht hinter einem Ladebildschirm jeden Ort von selbst (etwa 30-40 Minuten, am besten in der "
    "Station) und startet dann fertig im Titelbildschirm neu. Deine Spielstände werden nicht verändert. Was auf "
    "dieser Konsole schon vorbereitet ist, geht schneller.",
    "Später",
    "Diese Meldung erscheint nicht wieder. Grafik vorbereiten bleibt im Einstellungsmenü: Halte im Spiel Minus (-), "
    "Reiter Picture.",

    "SwitchWakerHD: Spieldateien nicht gefunden",
    "SwitchWakerHD braucht deine eigene Kopie des Spiels in %s/ (die Ordner code/, content/ und meta/), so wie "
    "tools/switch/make_sd.py sie aus deinem Dump anlegt (INSTALL.md).",
    "Fehlt: %s",
    "Kopiere den von make_sd.py erstellten Ordner build/sd/ ins Hauptverzeichnis der SD-Karte und starte das Spiel "
    "neu.",
    "Drücke + zum Beenden.",

    "Grafik wird vorbereitet",
    "%zu von %zu",
    "Start",
    "Noch etwa %d Min.",
    "Restzeit wird berechnet...",
    "Das Spiel besucht jeden Ort von selbst, um seine Grafik vorzubereiten, ohne Ton und Vibration. Danach startet es "
    "im Titelbildschirm neu. Deine Spielstände werden nicht verändert.",
    "Halt nach diesem Ort... Bisheriges bleibt erhalten.",
    "B weiter halten zum Anhalten",
    "Halte B zum Anhalten. Bisheriges bleibt erhalten, und es geht beim nächsten Mal hier weiter.",

    "Grafik vorbereiten (experimentell)",
    "Vorbereitung anhalten",
    "Bisher Vorbereitetes bleibt erhalten; beim nächsten Mal geht es hier weiter.",
    "Die Konsole bereitet jeden Grafikeffekt vor, wenn er zum ersten Mal gezeichnet wird; neue Orte können daher "
    "einige Sekunden schwarz sein oder Objekte verzögert zeigen. Grafik vorbereiten erledigt das für das ganze Spiel "
    "auf einmal: Das Spiel besucht jeden Ort von selbst (30-40 Minuten, am besten in der Station) und startet dann im "
    "Titelbildschirm neu. Deine Spielstände werden nicht verändert.",
    "Der Akku ist bei %u%%: Schließe zuerst das Ladegerät an oder stelle die Konsole in die Station.",
    "Vorbereitung fortsetzen (%zu von %zu erledigt)",
    "Grafik erneut vorbereiten",
    "Grafik vorbereiten",
    "Auf dieser Konsole erledigt. Erneut lohnt sich nur nach einem Update, das die Grafik ändert.",

    "Grafik bereit: Das ganze Spiel ist auf dieser Konsole vorbereitet.",
    "Grafik wird vorbereitet: %zu von %zu (%s)%s. Zum Anhalten: B halten.",
    ", noch etwa %d Min.",
    "Grafik wird vorbereitet: %s. Das Spiel startet neu.",
    "fertig",
    "angehalten (erneut starten, um fortzufahren)",
    "ein Ort wurde nicht geladen; erneut starten, um fortzufahren",
    "Vorbereitung fertig. Beende das Spiel und starte es neu (deine Spielstände sind unverändert).",
    "läuft bereits",
    "starte zuerst ein Spiel (dein Spielstand oder ein neues Spiel, das du nicht speicherst)",
    "die Spielstanddateien konnten nicht kopiert werden (ist die SD-Karte voll?)",
    "Grafik vorbereiten: Wähle deinen Spielstand (oder starte ein neues Spiel, das du nicht speicherst), dann beginnt es von selbst.",
    "Grafik wird vorbereitet",
    "Erster Start: Die Konsole bereitet die Grafik des Spiels einmal vor, damit Orte beim ersten Mal weder schwarz noch verzögert sind. Das dauert etwa 20 Minuten, am besten in der Station.",
    "Spiel wird gestartet...",
    "Grafik des Spiels wird gelesen...",
    "%zu von %zu",
    "B: jetzt spielen (der Rest läuft im Hintergrund weiter)",
    "Entfernt die schwarzen Schatten und fehlenden Effekte, die erscheinen, wenn ein Ort zum ersten Mal "
    "gezeichnet wird, weil die Konsole die Shader noch nicht im Cache hat, und sorgt danach für ein viel "
    "flüssigeres Spiel. Das Spiel besucht jeden Ort einmal von selbst (30-40 Minuten, am besten in der Station). "
    "Über 90 % der Shader lassen sich vorbereiten; einige wenige können nicht kompiliert werden.",
    "Nicht fertig: Ort %zu von %zu. Unten fortsetzen, um dort weiterzumachen.",
    "Auf dieser Konsole erledigt. Erneut lohnt sich nur nach einem Update, das die Grafik ändert.",
    "Auf dieser Konsole noch nicht erledigt.",
};

const Texts kItalian = {
    "SwitchWakerHD: primo avvio",
    "La grafica non è ancora stata preparata su questa console.",
    "In questo primo avvio alcune texture potrebbero apparire nere e alcuni oggetti comparire con un attimo di "
    "ritardo: la console prepara ogni effetto grafico la prima volta che viene disegnato. Migliora man mano che "
    "giochi, e ciò che è pronto viene conservato per gli avvii successivi.",
    "Prepara la grafica ora",
    "Poi scegli il tuo diario (o inizia una nuova partita che non salvi): il gioco visita da solo ogni luogo dietro "
    "una schermata di caricamento (circa 30-40 minuti, meglio con la console nella base) e riparte dalla schermata "
    "del titolo, pronto. I tuoi salvataggi non vengono toccati.",
    "Gioca ora",
    "La console prepara prima la grafica più comune dietro una schermata di caricamento (circa 20 minuti; B lì per "
    "giocare subito). Prepara la grafica resta nelle impostazioni: tieni premuto Meno (-) nel gioco, scheda Picture.",

    "SwitchWakerHD: novità in questa versione",
    "Prepara la grafica: ora la console può preparare in una volta la grafica di tutto il gioco, così i luoghi non "
    "ancora visitati non saranno neri né in ritardo la prima volta.",
    "Il gioco visita da solo ogni luogo dietro una schermata di caricamento (circa 30-40 minuti, meglio con la "
    "console nella base) e poi riparte dalla schermata del titolo, pronto. I tuoi salvataggi non vengono toccati. "
    "Ciò che è già pronto su questa console va più veloce.",
    "Più tardi",
    "Questo messaggio non apparirà più. Prepara la grafica resta nelle impostazioni: tieni premuto Meno (-) nel "
    "gioco, scheda Picture.",

    "SwitchWakerHD: file del gioco non trovati",
    "SwitchWakerHD ha bisogno della tua copia del gioco in %s/ (le cartelle code/, content/ e meta/), come la "
    "prepara tools/switch/make_sd.py dal tuo dump (INSTALL.md).",
    "Manca: %s",
    "Copia la cartella build/sd/ creata da make_sd.py nella radice della scheda SD, poi riavvia il gioco.",
    "Premi + per chiudere.",

    "Preparazione della grafica",
    "%zu di %zu",
    "avvio",
    "Circa %d min rimasti",
    "Calcolo del tempo rimanente...",
    "Il gioco sta visitando da solo ogni luogo per preparare la grafica, senza audio né vibrazione. Alla fine "
    "riparte dalla schermata del titolo. I tuoi salvataggi non vengono toccati.",
    "Arresto dopo questo luogo... Ciò che è fatto viene conservato.",
    "Continua a tenere B per fermare",
    "Tieni premuto B per fermare. Ciò che è fatto viene conservato e la prossima volta si riprende da qui.",

    "Prepara la grafica (sperimentale)",
    "Ferma la preparazione",
    "Ciò che è pronto viene conservato; la prossima volta si riprende da qui.",
    "La console prepara ogni effetto grafico la prima volta che viene disegnato, quindi i luoghi nuovi possono "
    "apparire neri o mostrare oggetti in ritardo per qualche secondo. Prepara la grafica lo fa per tutto il gioco in "
    "una volta: il gioco visita da solo ogni luogo (30-40 minuti, meglio con la console nella base) e poi riparte "
    "dalla schermata del titolo. I tuoi salvataggi non vengono toccati.",
    "La batteria è al %u%%: collega il caricatore o metti la console nella base prima.",
    "Continua la preparazione (%zu di %zu fatti)",
    "Prepara di nuovo la grafica",
    "Prepara la grafica",
    "Fatto su questa console. Ripeterlo serve solo dopo un aggiornamento che cambia la grafica.",

    "Grafica pronta: tutto il gioco è preparato su questa console.",
    "Preparazione della grafica: %zu di %zu (%s)%s. Per fermare: tieni B.",
    ", circa %d min rimasti",
    "Preparazione della grafica: %s. Il gioco si riavvia.",
    "completata",
    "fermata (riavviala per continuare)",
    "un luogo non si è caricato; riavviala per continuare",
    "Preparazione completata. Chiudi il gioco e riaprilo (i tuoi salvataggi sono intatti).",
    "già in corso",
    "avvia prima una partita (il tuo diario, o una nuova partita che non salvi)",
    "impossibile copiare i file del diario (la scheda SD è piena?)",
    "Prepara la grafica: scegli il tuo diario (o inizia una nuova partita che non salvi) e partirà da sola.",
    "Preparazione della grafica",
    "Primo avvio: la console prepara una volta la grafica del gioco, così i luoghi non saranno neri né in ritardo la prima volta. Circa 20 minuti, meglio con la console nella base.",
    "Avvio del gioco...",
    "Lettura della grafica del gioco...",
    "%zu di %zu",
    "B: gioca ora (il resto continua in background)",
    "Elimina le ombre nere e gli effetti mancanti che compaiono la prima volta che un luogo viene disegnato, "
    "perché la console non ha ancora gli shader in cache, e poi rende il gioco molto più fluido. Il gioco visita "
    "da solo ogni luogo una volta (30-40 minuti, meglio con la console nella base). Si può preparare oltre il 90% "
    "degli shader; alcuni non si possono compilare.",
    "Non finito: luogo %zu di %zu. Continua qui sotto per riprendere da lì.",
    "Fatto su questa console. Ripeterlo serve solo dopo un aggiornamento che cambia la grafica.",
    "Non ancora fatto su questa console.",
};

// the game's "language" setting (Wii U codes, hle/coreinit_misc.cpp), else the console's language
const MenuTexts kMenuEnglish = {
    // Warp tab
    .warpWarning = "Warning: warping can break the story. Going somewhere before the game gets there can leave events in "
        "that place stuck or out of order, and the game may save it. Try it on a copy of your save.",
    .warpUnlock = "I understand, show the warp destinations",
    .warpNowIn = "Now in: %.8s%s",
    .warpPending = "   (a warp is waiting for a loaded file or the current scene change)",
    .warpMain = "Main places",
    .warpAll = "Every stage",
    .warpRoomPoint = "%s  (room %d, point %d)",
    // Picture tab: resolution
    .resolution = "Resolution",
    .handheld = "Handheld",
    .docked = "Docked",
    .active = "  (active)",
    .modeHelp = "Each mode keeps its own internal resolution and dynamic resolution;\n"
        "they apply by themselves when the console is docked or undocked.",
    .dynamicRes = "Dynamic resolution",
    .dynamicResHelp = "Lowers the internal resolution while the GPU cannot hold 30 fps, and raises it again when it can.",
    .appliesDocked = "Applies when the console is docked.",
    .appliesHandheld = "Applies when the console is in handheld mode.",
    .resNow = "Internal resolution now: %.2f.",
    // Picture tab: colours and effects
    .colours = "Colours and effects",
    .exposure = "Exposure",
    .exposureHelp = "Below 1 tames bright areas (sky, sea)",
    .contrast = "Contrast",
    .saturation = "Saturation",
    .gamma = "Gamma",
    .gammaHelp = "Above 1 deepens mid-tones and shadows, below 1 lifts them",
    .originalColours = "Original colours",
    .vivid = "Vivid",
    .aniso = "16x anisotropic filtering",
    .anisoHelp = "Sharper ground, sand and water textures seen at an angle, mostly at a distance.\n"
        "Costs some GPU time: if the frame rate drops, turn it off.",
    .fxaa = "Edge smoothing (FXAA)",
    .fxaaHelp = "Smoother edges in the 3D world; the HUD stays sharp.\n"
        "Costs a little GPU time (about 2 ms a frame).",
    .bloom = "Bloom",
    .bloomHelp = "The glow around the sky and bright places, as on the Wii U.\n"
        "Costs a little GPU time.",
    .blur = "Distance blur",
    .blurHelp = "Far places softly out of focus, as on the Wii U.\n"
        "Costs a little GPU time.",
    // Picture tab: frame rate counter
    .fpsHeading = "Frame rate counter",
    .fpsOff = "Off",
    .fpsRate = "Frame rate",
    .fpsLoad = "Frame rate and load",
    .fpsOffTip = "No counter.",
    .fpsRateTip = "Frames per second, top left.",
    .fpsLoadTip = "Frames per second plus the render thread's load, draws and GPU lag.",
    .fpsHelp = "Shows the frames per second in the top left corner of the picture.",
    // Picture tab: experimental features
    .experimental = "Experimental features",
    .experimentalWarn = "Unfinished: 60 fps needs a big overclock (sys-clk or similar) to hold. Not saved.",
    .fps30 = "30 fps (original)",
    .fps30Tip = "The game as it was: 30 logic steps and 30 drawn frames a second.",
    .fpsInterp = "60 fps interpolation",
    .fpsInterpTip = "The game logic keeps 30 steps a second and a blended frame is drawn between them:\n"
        "doubles the drawing work, so it also needs a big overclock to hold 60 fps.",
    .fpsTrue60 = "True 60 (experimental)",
    .fpsTrue60Tip = "Runs the game logic at 60 steps a second. Needs a big CPU and GPU overclock (sys-clk) to hold 60 "
        "fps.",
    .fpsModesHelp = "Frame rate modes the game was not made for (the screen is 60 Hz). Not saved: 30 fps at the next "
        "start.",
    .keepSpeed = "Keep game speed",
    .keepSpeedHelp = "When the console cannot draw 60 frames a second, skip in-between frames instead of slowing\n"
        "the whole game down: the game logic keeps its 30 steps a second. Off: every in-between frame\n"
        "is drawn and the game runs slower than real time. Not saved: off at the next start.",
    .drawnLately = "In-between frames drawn lately: %.0f%%",
    // Controls tab
    .rumbleHeading = "Rumble",
    .rumble = "Rumble",
    .rumbleHelp = "The Joy-Con or the Pro Controller vibrates when the game asks for it, as the Wii U\n"
        "controllers did. Off keeps the motors still.",
    .gyroHeading = "Gyro aiming",
    .gyroAim = "Aim with the controller's gyro",
    .gyroAimHelp = "First-person aiming (bow, hookshot, boomerang, telescope, Picto Box, grappling hook) follows\n"
        "the Joy-Con, the Pro Controller or the console in handheld mode, as with the Wii U GamePad.\n"
        "The game's own Options > Gyro switch still applies, and the game ignores the motion while\n"
        "the right stick is pushed.",
    .turnBy = "Turn left/right by",
    .axisPlayer = "Player space",
    .axisYaw = "Yaw",
    .axisRoll = "Roll",
    .axisHelp = "Player space: turning the controller left or right about the real vertical, however you hold it\n"
        "(recommended). Yaw: turning it about its own vertical axis (as if it lay flat). Roll: tilting it "
        "to\n"
        "the side like a steering wheel. Tilting its top up or down always looks up or down.",
    .sensX = "Sensitivity left/right",
    .sensY = "Sensitivity up/down",
    .invert = "Invert",
    .defaultSens = "Default sensitivity",
    .recalibrate = "Recalibrate now",
    .recalibrateHelp = "Recalibrate if the view drifts while the controller rests, then put it down for a second:\n"
        "the gyro's offset is learnt anew.",
    .gyroReceiving = "Receiving motion.",
    .gyroCalibrating = "Receiving motion; calibrating (rest the controller for a second).",
    .gyroNoMotion = "A controller with a gyro is connected, no motion yet.",
    .gyroNone = "No connected controller has a gyro.",
    .gyroOff = "Gyro off.",
    .gyroStick = " The right stick is pushed: the game ignores the gyro until it is released.",
    .menuHeading = "Menu",
    .menuNote = "Minus held half a second opens this menu (a short Minus goes to the game); B or Minus closes it.\n"
        "L / R change tabs.",
    // Dev tab: what a player sees before ticking the box
    .devWarn = "Tests and diagnostics for developers. Nothing here is needed to play.",
    .devShow = "Show developer options",
    .devShowHelp = "Shows the tests and diagnostics below (debug server, A/B switches, captures). Off by default.\n"
        "A debug server that is already running keeps running whether this is ticked or not.",
};

const MenuTexts kMenuSpanish = {
    // Warp tab
    .warpWarning = "Atención: viajar puede romper la historia. Ir a un lugar antes de que el juego llegue allí puede "
        "dejar sus eventos atascados o desordenados, y el juego puede guardarlo. Pruébalo con una copia de tu "
        "partida.",
    .warpUnlock = "Entendido, mostrar los destinos",
    .warpNowIn = "Ahora en: %.8s%s",
    .warpPending = "   (un viaje espera a que se cargue una partida o termine el cambio de escena)",
    .warpMain = "Lugares principales",
    .warpAll = "Todas las zonas",
    .warpRoomPoint = "%s  (sala %d, punto %d)",
    // Picture tab: resolution
    .resolution = "Resolución",
    .handheld = "Portátil",
    .docked = "En la base",
    .active = "  (activo)",
    .modeHelp = "Cada modo guarda su propia resolución interna y su resolución dinámica;\n"
        "se aplican solas al poner o quitar la consola de la base.",
    .dynamicRes = "Resolución dinámica",
    .dynamicResHelp = "Baja la resolución interna mientras la GPU no puede mantener 30 fps, y la vuelve a subir cuando "
        "puede.",
    .appliesDocked = "Se aplica cuando la consola está en la base.",
    .appliesHandheld = "Se aplica cuando la consola está en modo portátil.",
    .resNow = "Resolución interna ahora: %.2f.",
    // Picture tab: colours and effects
    .colours = "Colores y efectos",
    .exposure = "Exposición",
    .exposureHelp = "Por debajo de 1 suaviza las zonas brillantes (cielo, mar)",
    .contrast = "Contraste",
    .saturation = "Saturación",
    .gamma = "Gamma",
    .gammaHelp = "Por encima de 1 oscurece los tonos medios y las sombras; por debajo de 1 los aclara",
    .originalColours = "Colores originales",
    .vivid = "Vivos",
    .aniso = "Filtrado anisotrópico 16x",
    .anisoHelp = "Texturas de suelo, arena y agua más nítidas vistas en ángulo, sobre todo a lo lejos.\n"
        "Cuesta algo de GPU: si bajan los fps, desactívalo.",
    .fxaa = "Suavizado de bordes (FXAA)",
    .fxaaHelp = "Bordes más suaves en el mundo 3D; el HUD sigue nítido.\n"
        "Cuesta un poco de GPU (unos 2 ms por fotograma).",
    .bloom = "Resplandor (bloom)",
    .bloomHelp = "El brillo alrededor del cielo y de los lugares luminosos, como en Wii U.\n"
        "Cuesta un poco de GPU.",
    .blur = "Desenfoque lejano",
    .blurHelp = "Los lugares lejanos, ligeramente desenfocados, como en Wii U.\n"
        "Cuesta un poco de GPU.",
    // Picture tab: frame rate counter
    .fpsHeading = "Contador de fps",
    .fpsOff = "No",
    .fpsRate = "Fps",
    .fpsLoad = "Fps y carga",
    .fpsOffTip = "Sin contador.",
    .fpsRateTip = "Fotogramas por segundo, arriba a la izquierda.",
    .fpsLoadTip = "Fotogramas por segundo más la carga del hilo de render, las llamadas de dibujo y el retraso de la "
        "GPU.",
    .fpsHelp = "Muestra los fotogramas por segundo en la esquina superior izquierda de la imagen.",
    // Picture tab: experimental features
    .experimental = "Funciones experimentales",
    .experimentalWarn = "Sin terminar: mantener 60 fps necesita un gran overclock (sys-clk o similar). No se guarda.",
    .fps30 = "30 fps (original)",
    .fps30Tip = "El juego tal como era: 30 pasos de lógica y 30 fotogramas dibujados por segundo.",
    .fpsInterp = "Interpolación a 60 fps",
    .fpsInterpTip = "La lógica del juego sigue a 30 pasos por segundo y se dibuja un fotograma mezclado entre ellos:\n"
        "duplica el trabajo de dibujo, así que también necesita un gran overclock para mantener 60 fps.",
    .fpsTrue60 = "60 reales (experimental)",
    .fpsTrue60Tip = "Ejecuta la lógica del juego a 60 pasos por segundo. Necesita un gran overclock de CPU y GPU "
        "(sys-clk) para mantener 60 fps.",
    .fpsModesHelp = "Modos de fps para los que el juego no se hizo (la pantalla va a 60 Hz). No se guarda: 30 fps en el "
        "siguiente arranque.",
    .keepSpeed = "Mantener la velocidad del juego",
    .keepSpeedHelp = "Cuando la consola no puede dibujar 60 fotogramas por segundo, se saltan fotogramas intermedios en "
        "vez de\n"
        "ralentizar todo el juego: la lógica mantiene sus 30 pasos por segundo. Desactivado: se dibujan todos "
        "los\n"
        "intermedios y el juego va más lento que el tiempo real. No se guarda: desactivado en el siguiente "
        "arranque.",
    .drawnLately = "Fotogramas intermedios dibujados últimamente: %.0f%%",
    // Controls tab
    .rumbleHeading = "Vibración",
    .rumble = "Vibración",
    .rumbleHelp = "Los Joy-Con o el mando Pro vibran cuando el juego lo pide, como los mandos de Wii U.\n"
        "Desactivado, los motores no se mueven.",
    .gyroHeading = "Apuntado con giroscopio",
    .gyroAim = "Apuntar con el giroscopio del mando",
    .gyroAimHelp = "El apuntado en primera persona (arco, gancho, bumerán, catalejo, Pictógrafo, garfio) sigue\n"
        "a los Joy-Con, al mando Pro o a la consola en modo portátil, como con el GamePad de Wii U.\n"
        "La opción Giroscopio del propio juego sigue contando, y el juego ignora el movimiento mientras\n"
        "se empuja el stick derecho.",
    .turnBy = "Girar a izquierda/derecha con",
    .axisPlayer = "Espacio del jugador",
    .axisYaw = "Guiñada",
    .axisRoll = "Alabeo",
    .axisHelp = "Espacio del jugador: girar el mando a izquierda o derecha sobre la vertical real, lo sujetes como lo "
        "sujetes\n"
        "(recomendado). Guiñada: girarlo sobre su propio eje vertical (como si estuviera tumbado). Alabeo: "
        "inclinarlo\n"
        "hacia un lado como un volante. Inclinar la parte de arriba hacia arriba o abajo siempre mira arriba "
        "o abajo.",
    .sensX = "Sensibilidad izquierda/derecha",
    .sensY = "Sensibilidad arriba/abajo",
    .invert = "Invertir",
    .defaultSens = "Sensibilidad por defecto",
    .recalibrate = "Recalibrar ahora",
    .recalibrateHelp = "Recalibra si la vista se desplaza con el mando quieto, y luego déjalo apoyado un segundo:\n"
        "el giroscopio vuelve a aprender su desviación.",
    .gyroReceiving = "Recibiendo movimiento.",
    .gyroCalibrating = "Recibiendo movimiento; calibrando (deja el mando quieto un segundo).",
    .gyroNoMotion = "Hay un mando con giroscopio conectado; aún no llega movimiento.",
    .gyroNone = "Ningún mando conectado tiene giroscopio.",
    .gyroOff = "Giroscopio desactivado.",
    .gyroStick = " El stick derecho está empujado: el juego ignora el giroscopio hasta que lo sueltes.",
    .menuHeading = "Menú",
    .menuNote = "Mantener Menos medio segundo abre este menú (una pulsación corta de Menos va al juego); B o Menos lo "
        "cierran.\n"
        "L / R cambian de pestaña.",
    // Dev tab: what a player sees before ticking the box
    .devWarn = "Pruebas y diagnósticos para desarrolladores. Nada de aquí hace falta para jugar.",
    .devShow = "Mostrar opciones de desarrollador",
    .devShowHelp = "Muestra debajo las pruebas y diagnósticos (servidor de depuración, interruptores A/B, capturas). "
        "Desactivado por defecto.\n"
        "Un servidor de depuración que ya esté en marcha sigue funcionando, esté marcado o no.",
};

const MenuTexts kMenuFrench = {
    // Warp tab
    .warpWarning = "Attention : se téléporter peut casser l'histoire. Aller dans un lieu avant que le jeu n'y arrive "
        "peut bloquer ses événements ou les mélanger, et le jeu peut l'enregistrer. Essayez-le sur une copie "
        "de votre sauvegarde.",
    .warpUnlock = "J'ai compris, afficher les destinations",
    .warpNowIn = "Lieu actuel : %.8s%s",
    .warpPending = "   (une téléportation attend une partie chargée ou la fin du changement de scène)",
    .warpMain = "Lieux principaux",
    .warpAll = "Toutes les zones",
    .warpRoomPoint = "%s  (salle %d, point %d)",
    // Picture tab: resolution
    .resolution = "Résolution",
    .handheld = "Portable",
    .docked = "Sur la station",
    .active = "  (actif)",
    .modeHelp = "Chaque mode garde sa propre résolution interne et sa résolution dynamique ;\n"
        "elles s'appliquent d'elles-mêmes quand la console est posée sur la station ou retirée.",
    .dynamicRes = "Résolution dynamique",
    .dynamicResHelp = "Baisse la résolution interne tant que le GPU ne tient pas 30 fps, et la remonte quand il le peut.",
    .appliesDocked = "S'applique quand la console est sur la station.",
    .appliesHandheld = "S'applique quand la console est en mode portable.",
    .resNow = "Résolution interne actuelle : %.2f.",
    // Picture tab: colours and effects
    .colours = "Couleurs et effets",
    .exposure = "Exposition",
    .exposureHelp = "En dessous de 1, adoucit les zones claires (ciel, mer)",
    .contrast = "Contraste",
    .saturation = "Saturation",
    .gamma = "Gamma",
    .gammaHelp = "Au-dessus de 1, assombrit les tons moyens et les ombres ; en dessous de 1, les éclaircit",
    .originalColours = "Couleurs d'origine",
    .vivid = "Vives",
    .aniso = "Filtrage anisotrope 16x",
    .anisoHelp = "Textures du sol, du sable et de l'eau plus nettes vues de biais, surtout au loin.\n"
        "Coûte du temps GPU : si les fps baissent, désactivez-le.",
    .fxaa = "Lissage des contours (FXAA)",
    .fxaaHelp = "Contours plus doux dans le monde 3D ; l'interface reste nette.\n"
        "Coûte un peu de temps GPU (environ 2 ms par image).",
    .bloom = "Halo lumineux (bloom)",
    .bloomHelp = "La lueur autour du ciel et des endroits lumineux, comme sur Wii U.\n"
        "Coûte un peu de temps GPU.",
    .blur = "Flou de distance",
    .blurHelp = "Les lieux lointains légèrement flous, comme sur Wii U.\n"
        "Coûte un peu de temps GPU.",
    // Picture tab: frame rate counter
    .fpsHeading = "Compteur de fps",
    .fpsOff = "Non",
    .fpsRate = "Fps",
    .fpsLoad = "Fps et charge",
    .fpsOffTip = "Pas de compteur.",
    .fpsRateTip = "Images par seconde, en haut à gauche.",
    .fpsLoadTip = "Images par seconde, plus la charge du thread de rendu, les appels de dessin et le retard du GPU.",
    .fpsHelp = "Affiche les images par seconde dans le coin supérieur gauche de l'image.",
    // Picture tab: experimental features
    .experimental = "Fonctions expérimentales",
    .experimentalWarn = "Inachevé : tenir 60 fps demande un fort overclock (sys-clk ou équivalent). Non enregistré.",
    .fps30 = "30 fps (d'origine)",
    .fps30Tip = "Le jeu tel qu'il était : 30 étapes de logique et 30 images dessinées par seconde.",
    .fpsInterp = "Interpolation à 60 fps",
    .fpsInterpTip = "La logique du jeu garde 30 étapes par seconde et une image intermédiaire est dessinée entre elles "
        ":\n"
        "le travail de dessin double, il faut donc aussi un fort overclock pour tenir 60 fps.",
    .fpsTrue60 = "60 réels (expérimental)",
    .fpsTrue60Tip = "Fait tourner la logique du jeu à 60 étapes par seconde. Demande un fort overclock du CPU et du GPU "
        "(sys-clk) pour tenir 60 fps.",
    .fpsModesHelp = "Modes de fps pour lesquels le jeu n'a pas été conçu (l'écran est à 60 Hz). Non enregistré : 30 fps "
        "au prochain démarrage.",
    .keepSpeed = "Garder la vitesse du jeu",
    .keepSpeedHelp = "Quand la console ne peut pas dessiner 60 images par seconde, des images intermédiaires sont sautées "
        "au lieu de\n"
        "ralentir tout le jeu : la logique garde ses 30 étapes par seconde. Désactivé : chaque image "
        "intermédiaire\n"
        "est dessinée et le jeu tourne plus lentement que le temps réel. Non enregistré : désactivé au "
        "prochain démarrage.",
    .drawnLately = "Images intermédiaires dessinées récemment : %.0f%%",
    // Controls tab
    .rumbleHeading = "Vibrations",
    .rumble = "Vibrations",
    .rumbleHelp = "Les Joy-Con ou la manette Pro vibrent quand le jeu le demande, comme les manettes\n"
        "Wii U. Désactivé, les moteurs restent immobiles.",
    .gyroHeading = "Visée gyroscopique",
    .gyroAim = "Viser avec le gyroscope de la manette",
    .gyroAimHelp = "La visée à la première personne (arc, grappin, boomerang, longue-vue, Picto-box, crochet) suit\n"
        "les Joy-Con, la manette Pro ou la console en mode portable, comme avec le GamePad Wii U.\n"
        "L'option Gyroscope du jeu s'applique toujours, et le jeu ignore le mouvement tant que\n"
        "le stick droit est poussé.",
    .turnBy = "Tourner à gauche/droite par",
    .axisPlayer = "Espace du joueur",
    .axisYaw = "Lacet",
    .axisRoll = "Roulis",
    .axisHelp = "Espace du joueur : tourner la manette à gauche ou à droite autour de la verticale réelle, quelle que "
        "soit la prise\n"
        "(recommandé). Lacet : la tourner autour de son propre axe vertical (comme posée à plat). Roulis : "
        "l'incliner\n"
        "sur le côté comme un volant. Incliner le haut vers le haut ou le bas regarde toujours en haut ou en "
        "bas.",
    .sensX = "Sensibilité gauche/droite",
    .sensY = "Sensibilité haut/bas",
    .invert = "Inverser",
    .defaultSens = "Sensibilité par défaut",
    .recalibrate = "Recalibrer maintenant",
    .recalibrateHelp = "Recalibrez si la vue dérive manette immobile, puis posez-la une seconde :\n"
        "le décalage du gyroscope est réappris.",
    .gyroReceiving = "Mouvement reçu.",
    .gyroCalibrating = "Mouvement reçu ; calibrage (laissez la manette immobile une seconde).",
    .gyroNoMotion = "Une manette avec gyroscope est connectée, pas encore de mouvement.",
    .gyroNone = "Aucune manette connectée n'a de gyroscope.",
    .gyroOff = "Gyroscope désactivé.",
    .gyroStick = " Le stick droit est poussé : le jeu ignore le gyroscope jusqu'à ce qu'il soit relâché.",
    .menuHeading = "Menu",
    .menuNote = "Maintenir Moins une demi-seconde ouvre ce menu (un appui court sur Moins va au jeu) ; B ou Moins le "
        "ferme.\n"
        "L / R changent d'onglet.",
    // Dev tab: what a player sees before ticking the box
    .devWarn = "Tests et diagnostics pour les développeurs. Rien ici n'est nécessaire pour jouer.",
    .devShow = "Afficher les options développeur",
    .devShowHelp = "Affiche ci-dessous les tests et diagnostics (serveur de débogage, interrupteurs A/B, captures). "
        "Désactivé par défaut.\n"
        "Un serveur de débogage déjà lancé continue de tourner, que la case soit cochée ou non.",
};

const MenuTexts kMenuGerman = {
    // Warp tab
    .warpWarning = "Achtung: Teleportieren kann die Geschichte kaputt machen. Wer einen Ort erreicht, bevor das Spiel "
        "dort ankommt, kann dessen Ereignisse blockieren oder durcheinanderbringen, und das Spiel kann das "
        "speichern. Probiere es mit einer Kopie deines Spielstands.",
    .warpUnlock = "Verstanden, Ziele anzeigen",
    .warpNowIn = "Jetzt in: %.8s%s",
    .warpPending = "   (ein Teleport wartet auf einen geladenen Spielstand oder den laufenden Szenenwechsel)",
    .warpMain = "Wichtige Orte",
    .warpAll = "Alle Bereiche",
    .warpRoomPoint = "%s  (Raum %d, Punkt %d)",
    // Picture tab: resolution
    .resolution = "Auflösung",
    .handheld = "Handheld",
    .docked = "In der Station",
    .active = "  (aktiv)",
    .modeHelp = "Jeder Modus behält seine eigene interne und dynamische Auflösung;\n"
        "sie gelten von selbst, wenn die Konsole in die Station gestellt oder herausgenommen wird.",
    .dynamicRes = "Dynamische Auflösung",
    .dynamicResHelp = "Senkt die interne Auflösung, solange die GPU keine 30 fps halten kann, und hebt sie wieder an, "
        "sobald sie es kann.",
    .appliesDocked = "Gilt, wenn die Konsole in der Station steht.",
    .appliesHandheld = "Gilt, wenn die Konsole im Handheld-Modus ist.",
    .resNow = "Interne Auflösung jetzt: %.2f.",
    // Picture tab: colours and effects
    .colours = "Farben und Effekte",
    .exposure = "Belichtung",
    .exposureHelp = "Unter 1 dämpft helle Bereiche (Himmel, Meer)",
    .contrast = "Kontrast",
    .saturation = "Sättigung",
    .gamma = "Gamma",
    .gammaHelp = "Über 1 vertieft Mitteltöne und Schatten, unter 1 hellt sie auf",
    .originalColours = "Originalfarben",
    .vivid = "Kräftig",
    .aniso = "16x anisotrope Filterung",
    .anisoHelp = "Schärfere Boden-, Sand- und Wassertexturen im schrägen Blick, vor allem in der Ferne.\n"
        "Kostet GPU-Zeit: Wenn die Bildrate sinkt, schalte es aus.",
    .fxaa = "Kantenglättung (FXAA)",
    .fxaaHelp = "Weichere Kanten in der 3D-Welt; die Anzeige bleibt scharf.\n"
        "Kostet etwas GPU-Zeit (etwa 2 ms pro Bild).",
    .bloom = "Leuchten (Bloom)",
    .bloomHelp = "Das Leuchten um den Himmel und helle Orte, wie auf der Wii U.\n"
        "Kostet etwas GPU-Zeit.",
    .blur = "Ferne Unschärfe",
    .blurHelp = "Ferne Orte leicht unscharf, wie auf der Wii U.\n"
        "Kostet etwas GPU-Zeit.",
    // Picture tab: frame rate counter
    .fpsHeading = "Bildratenanzeige",
    .fpsOff = "Aus",
    .fpsRate = "Bildrate",
    .fpsLoad = "Bildrate und Last",
    .fpsOffTip = "Keine Anzeige.",
    .fpsRateTip = "Bilder pro Sekunde, oben links.",
    .fpsLoadTip = "Bilder pro Sekunde plus Last des Render-Threads, Zeichenaufrufe und GPU-Verzögerung.",
    .fpsHelp = "Zeigt die Bilder pro Sekunde in der oberen linken Ecke des Bildes.",
    // Picture tab: experimental features
    .experimental = "Experimentelle Funktionen",
    .experimentalWarn = "Unfertig: 60 fps zu halten braucht starkes Übertakten (sys-clk o. Ä.). Wird nicht gespeichert.",
    .fps30 = "30 fps (original)",
    .fps30Tip = "Das Spiel wie es war: 30 Logikschritte und 30 gezeichnete Bilder pro Sekunde.",
    .fpsInterp = "60-fps-Interpolation",
    .fpsInterpTip = "Die Spiellogik bleibt bei 30 Schritten pro Sekunde, dazwischen wird ein gemischtes Bild "
        "gezeichnet:\n"
        "das verdoppelt die Zeichenarbeit, daher braucht es ebenfalls starkes Übertakten für 60 fps.",
    .fpsTrue60 = "Echte 60 (experimentell)",
    .fpsTrue60Tip = "Lässt die Spiellogik mit 60 Schritten pro Sekunde laufen. Braucht starkes CPU- und GPU-Übertakten "
        "(sys-clk), um 60 fps zu halten.",
    .fpsModesHelp = "Bildratenmodi, für die das Spiel nicht gemacht wurde (der Bildschirm hat 60 Hz). Nicht gespeichert: "
        "30 fps beim nächsten Start.",
    .keepSpeed = "Spieltempo halten",
    .keepSpeedHelp = "Wenn die Konsole keine 60 Bilder pro Sekunde zeichnen kann, werden Zwischenbilder übersprungen, "
        "statt das\n"
        "ganze Spiel zu verlangsamen: Die Logik behält ihre 30 Schritte pro Sekunde. Aus: Jedes Zwischenbild "
        "wird\n"
        "gezeichnet und das Spiel läuft langsamer als Echtzeit. Nicht gespeichert: aus beim nächsten Start.",
    .drawnLately = "Zuletzt gezeichnete Zwischenbilder: %.0f%%",
    // Controls tab
    .rumbleHeading = "Vibration",
    .rumble = "Vibration",
    .rumbleHelp = "Die Joy-Con oder der Pro Controller vibrieren, wenn das Spiel es verlangt, wie die\n"
        "Wii-U-Controller. Aus: Die Motoren bleiben still.",
    .gyroHeading = "Gyro-Zielen",
    .gyroAim = "Mit dem Gyroskop des Controllers zielen",
    .gyroAimHelp = "Das Zielen in der Ich-Perspektive (Bogen, Enterhaken, Bumerang, Fernrohr, Piktobox, Greifhaken)\n"
        "folgt den Joy-Con, dem Pro Controller oder der Konsole im Handheld-Modus, wie mit dem Wii U "
        "GamePad.\n"
        "Die Gyro-Option des Spiels gilt weiterhin, und das Spiel ignoriert die Bewegung, solange der\n"
        "rechte Stick gedrückt wird.",
    .turnBy = "Links/rechts drehen per",
    .axisPlayer = "Spielerraum",
    .axisYaw = "Gieren",
    .axisRoll = "Rollen",
    .axisHelp = "Spielerraum: den Controller um die echte Senkrechte nach links oder rechts drehen, egal wie du ihn "
        "hältst\n"
        "(empfohlen). Gieren: ihn um seine eigene Hochachse drehen (als läge er flach). Rollen: ihn wie ein "
        "Lenkrad\n"
        "zur Seite neigen. Die Oberseite nach oben oder unten kippen schaut immer nach oben oder unten.",
    .sensX = "Empfindlichkeit links/rechts",
    .sensY = "Empfindlichkeit oben/unten",
    .invert = "Umkehren",
    .defaultSens = "Standardempfindlichkeit",
    .recalibrate = "Jetzt neu kalibrieren",
    .recalibrateHelp = "Neu kalibrieren, wenn die Ansicht wandert, obwohl der Controller ruht, dann ihn eine Sekunde "
        "ablegen:\n"
        "Der Versatz des Gyroskops wird neu gelernt.",
    .gyroReceiving = "Bewegung wird empfangen.",
    .gyroCalibrating = "Bewegung wird empfangen; Kalibrierung (lege den Controller eine Sekunde ab).",
    .gyroNoMotion = "Ein Controller mit Gyroskop ist verbunden, noch keine Bewegung.",
    .gyroNone = "Kein verbundener Controller hat ein Gyroskop.",
    .gyroOff = "Gyroskop aus.",
    .gyroStick = " Der rechte Stick ist gedrückt: Das Spiel ignoriert das Gyroskop, bis er losgelassen wird.",
    .menuHeading = "Menü",
    .menuNote = "Minus eine halbe Sekunde halten öffnet dieses Menü (kurz Minus geht ans Spiel); B oder Minus "
        "schließt es.\n"
        "L / R wechseln den Reiter.",
    // Dev tab: what a player sees before ticking the box
    .devWarn = "Tests und Diagnosen für Entwickler. Nichts hier wird zum Spielen gebraucht.",
    .devShow = "Entwickleroptionen anzeigen",
    .devShowHelp = "Zeigt unten die Tests und Diagnosen (Debug-Server, A/B-Schalter, Aufnahmen). Standardmäßig aus.\n"
        "Ein bereits laufender Debug-Server läuft weiter, ob dies angehakt ist oder nicht.",
};

const MenuTexts kMenuItalian = {
    // Warp tab
    .warpWarning = "Attenzione: teletrasportarsi può rompere la storia. Andare in un luogo prima che il gioco ci arrivi "
        "può bloccarne gli eventi o metterli in disordine, e il gioco può salvarlo. Provalo su una copia del "
        "tuo salvataggio.",
    .warpUnlock = "Ho capito, mostra le destinazioni",
    .warpNowIn = "Ora in: %.8s%s",
    .warpPending = "   (un teletrasporto attende un salvataggio caricato o il cambio di scena in corso)",
    .warpMain = "Luoghi principali",
    .warpAll = "Tutte le zone",
    .warpRoomPoint = "%s  (stanza %d, punto %d)",
    // Picture tab: resolution
    .resolution = "Risoluzione",
    .handheld = "Portatile",
    .docked = "Nella base",
    .active = "  (attivo)",
    .modeHelp = "Ogni modalità conserva la propria risoluzione interna e dinamica;\n"
        "si applicano da sole quando la console viene messa nella base o tolta.",
    .dynamicRes = "Risoluzione dinamica",
    .dynamicResHelp = "Abbassa la risoluzione interna finché la GPU non riesce a tenere 30 fps, e la rialza quando ci "
        "riesce.",
    .appliesDocked = "Si applica quando la console è nella base.",
    .appliesHandheld = "Si applica quando la console è in modalità portatile.",
    .resNow = "Risoluzione interna ora: %.2f.",
    // Picture tab: colours and effects
    .colours = "Colori ed effetti",
    .exposure = "Esposizione",
    .exposureHelp = "Sotto 1 attenua le zone luminose (cielo, mare)",
    .contrast = "Contrasto",
    .saturation = "Saturazione",
    .gamma = "Gamma",
    .gammaHelp = "Sopra 1 scurisce i toni medi e le ombre, sotto 1 li schiarisce",
    .originalColours = "Colori originali",
    .vivid = "Vivaci",
    .aniso = "Filtro anisotropico 16x",
    .anisoHelp = "Texture di terreno, sabbia e acqua più nitide viste di sbieco, soprattutto in lontananza.\n"
        "Costa tempo di GPU: se gli fps calano, disattivalo.",
    .fxaa = "Smussatura dei bordi (FXAA)",
    .fxaaHelp = "Bordi più morbidi nel mondo 3D; l'interfaccia resta nitida.\n"
        "Costa un po' di tempo di GPU (circa 2 ms per fotogramma).",
    .bloom = "Bagliore (bloom)",
    .bloomHelp = "Il bagliore intorno al cielo e ai luoghi luminosi, come su Wii U.\n"
        "Costa un po' di tempo di GPU.",
    .blur = "Sfocatura in lontananza",
    .blurHelp = "I luoghi lontani leggermente sfocati, come su Wii U.\n"
        "Costa un po' di tempo di GPU.",
    // Picture tab: frame rate counter
    .fpsHeading = "Contatore di fps",
    .fpsOff = "No",
    .fpsRate = "Fps",
    .fpsLoad = "Fps e carico",
    .fpsOffTip = "Nessun contatore.",
    .fpsRateTip = "Fotogrammi al secondo, in alto a sinistra.",
    .fpsLoadTip = "Fotogrammi al secondo più il carico del thread di rendering, le chiamate di disegno e il ritardo "
        "della GPU.",
    .fpsHelp = "Mostra i fotogrammi al secondo nell'angolo in alto a sinistra dell'immagine.",
    // Picture tab: experimental features
    .experimental = "Funzioni sperimentali",
    .experimentalWarn = "Incompleto: tenere 60 fps richiede un forte overclock (sys-clk o simili). Non viene salvato.",
    .fps30 = "30 fps (originale)",
    .fps30Tip = "Il gioco com'era: 30 passi di logica e 30 fotogrammi disegnati al secondo.",
    .fpsInterp = "Interpolazione a 60 fps",
    .fpsInterpTip = "La logica del gioco resta a 30 passi al secondo e tra un passo e l'altro si disegna un fotogramma "
        "fuso:\n"
        "raddoppia il lavoro di disegno, quindi serve anche un forte overclock per tenere 60 fps.",
    .fpsTrue60 = "60 reali (sperimentale)",
    .fpsTrue60Tip = "Esegue la logica del gioco a 60 passi al secondo. Serve un forte overclock di CPU e GPU (sys-clk) "
        "per tenere 60 fps.",
    .fpsModesHelp = "Modalità di fps per cui il gioco non è stato fatto (lo schermo è a 60 Hz). Non salvato: 30 fps al "
        "prossimo avvio.",
    .keepSpeed = "Mantieni la velocità del gioco",
    .keepSpeedHelp = "Quando la console non riesce a disegnare 60 fotogrammi al secondo, salta i fotogrammi intermedi "
        "invece di\n"
        "rallentare tutto il gioco: la logica mantiene i suoi 30 passi al secondo. Disattivato: ogni "
        "fotogramma intermedio\n"
        "viene disegnato e il gioco va più lento del tempo reale. Non salvato: disattivato al prossimo avvio.",
    .drawnLately = "Fotogrammi intermedi disegnati di recente: %.0f%%",
    // Controls tab
    .rumbleHeading = "Vibrazione",
    .rumble = "Vibrazione",
    .rumbleHelp = "I Joy-Con o il controller Pro vibrano quando il gioco lo chiede, come i controller\n"
        "Wii U. Disattivato, i motori restano fermi.",
    .gyroHeading = "Mira giroscopica",
    .gyroAim = "Mira con il giroscopio del controller",
    .gyroAimHelp = "La mira in prima persona (arco, rampino, boomerang, cannocchiale, Pictobox, uncino) segue\n"
        "i Joy-Con, il controller Pro o la console in modalità portatile, come con il GamePad Wii U.\n"
        "L'opzione Giroscopio del gioco vale ancora, e il gioco ignora il movimento mentre\n"
        "lo stick destro è spinto.",
    .turnBy = "Girare a sinistra/destra con",
    .axisPlayer = "Spazio del giocatore",
    .axisYaw = "Imbardata",
    .axisRoll = "Rollio",
    .axisHelp = "Spazio del giocatore: girare il controller a sinistra o a destra attorno alla verticale reale, "
        "comunque lo tieni\n"
        "(consigliato). Imbardata: girarlo attorno al proprio asse verticale (come se fosse appoggiato). "
        "Rollio: inclinarlo\n"
        "di lato come un volante. Inclinare la parte alta in su o in giù guarda sempre in su o in giù.",
    .sensX = "Sensibilità sinistra/destra",
    .sensY = "Sensibilità su/giù",
    .invert = "Inverti",
    .defaultSens = "Sensibilità predefinita",
    .recalibrate = "Ricalibra ora",
    .recalibrateHelp = "Ricalibra se la visuale scorre con il controller fermo, poi appoggialo per un secondo:\n"
        "lo scostamento del giroscopio viene appreso di nuovo.",
    .gyroReceiving = "Movimento ricevuto.",
    .gyroCalibrating = "Movimento ricevuto; calibrazione (tieni fermo il controller per un secondo).",
    .gyroNoMotion = "Un controller con giroscopio è collegato, ancora nessun movimento.",
    .gyroNone = "Nessun controller collegato ha un giroscopio.",
    .gyroOff = "Giroscopio disattivato.",
    .gyroStick = " Lo stick destro è spinto: il gioco ignora il giroscopio finché non viene rilasciato.",
    .menuHeading = "Menu",
    .menuNote = "Tenere premuto Meno per mezzo secondo apre questo menu (una pressione breve di Meno va al gioco); B "
        "o Meno lo chiude.\n"
        "L / R cambiano scheda.",
    // Dev tab: what a player sees before ticking the box
    .devWarn = "Test e diagnostica per sviluppatori. Niente qui serve per giocare.",
    .devShow = "Mostra le opzioni sviluppatore",
    .devShowHelp = "Mostra qui sotto test e diagnostica (server di debug, interruttori A/B, catture). Disattivato per "
        "impostazione predefinita.\n"
        "Un server di debug già avviato continua a funzionare, che la casella sia spuntata o no.",
};

// the game's language code (1 en, 2 fr, 3 de, 4 it, 5 es; others as English)
long language() {
    std::string saved;
    long code = -1;
    if (hostui::get("language", saved) && !saved.empty()) code = strtol(saved.c_str(), nullptr, 10);
    if (code < 0) {
        u64 lc = 0;
        SetLanguage lang = SetLanguage_ENUS;
        if (R_SUCCEEDED(setInitialize())) {
            if (R_SUCCEEDED(setGetSystemLanguage(&lc)) && R_SUCCEEDED(setMakeLanguage(lc, &lang))) {
                switch (lang) {
                case SetLanguage_FR: case SetLanguage_FRCA: code = 2; break;
                case SetLanguage_DE: code = 3; break;
                case SetLanguage_IT: code = 4; break;
                case SetLanguage_ES: case SetLanguage_ES419: code = 5; break;
                default: code = 1; break;
                }
            }
            setExit();
        }
    }
    return code;
}

const Texts* choose() {
    switch (language()) {
    case 2: return &kFrench;
    case 3: return &kGerman;
    case 4: return &kItalian;
    case 5: return &kSpanish;
    default: return &kEnglish;
    }
}
const MenuTexts* choose_menu() {
    switch (language()) {
    case 2: return &kMenuFrench;
    case 3: return &kMenuGerman;
    case 4: return &kMenuItalian;
    case 5: return &kMenuSpanish;
    default: return &kMenuEnglish;
    }
}

}  // namespace

const Texts& tx() {
    static const Texts* t = choose();
    return *t;
}

const MenuTexts& menu() {
    // every field is a const char*: one a language leaves out (nullptr) is the English one
    static_assert(sizeof(MenuTexts) % sizeof(const char*) == 0, "MenuTexts holds only const char* fields");
    static const MenuTexts m = [] {
        MenuTexts out = *choose_menu();
        auto* dst = reinterpret_cast<const char**>(&out);
        auto* en = reinterpret_cast<const char* const*>(&kMenuEnglish);
        for (size_t i = 0; i < sizeof out / sizeof(const char*); i++)
            if (!dst[i]) dst[i] = en[i];
        return out;
    }();
    return m;
}

std::string to_console(const std::string& s) {
    // code page 437's accented letters
    static const struct { uint32_t cp; char c; } kMap[] = {
        {0xE1, char(0xA0)}, {0xE9, char(0x82)}, {0xED, char(0xA1)}, {0xF3, char(0xA2)}, {0xFA, char(0xA3)},
        {0xF1, char(0xA4)}, {0xD1, char(0xA5)}, {0xFC, char(0x81)}, {0xDC, char(0x9A)}, {0xE7, char(0x87)},
        {0xC7, char(0x80)}, {0xE0, char(0x85)}, {0xE8, char(0x8A)}, {0xEC, char(0x8D)}, {0xF2, char(0x95)},
        {0xF9, char(0x97)}, {0xE2, char(0x83)}, {0xEA, char(0x88)}, {0xEE, char(0x8C)}, {0xF4, char(0x93)},
        {0xFB, char(0x96)}, {0xE4, char(0x84)}, {0xC4, char(0x8E)}, {0xF6, char(0x94)}, {0xD6, char(0x99)},
        {0xDF, char(0xE1)}, {0xEB, char(0x89)}, {0xEF, char(0x8B)}, {0xC9, char(0x90)}, {0xBF, char(0xA8)},
        {0xA1, char(0xAD)}, {0xAB, char(0xAE)}, {0xBB, char(0xAF)}, {0xC0, 'A'}, {0xC8, 'E'}, {0xCA, 'E'},
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = uint8_t(s[i]);
        if (b < 0x80) { out += char(b); i++; continue; }
        uint32_t cp = 0;
        size_t n = 1;
        if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) cp = (b & 0x1F) << 6 | (uint8_t(s[i + 1]) & 0x3F), n = 2;
        else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) cp = 0x10000, n = 3;  // (none of ours: '?')
        else if ((b & 0xF8) == 0xF0) cp = 0x10000, n = 4;
        char c = '?';
        for (const auto& m : kMap)
            if (m.cp == cp) c = m.c;
        out += c;
        i += n;
    }
    return out;
}

}  // namespace ui_text
