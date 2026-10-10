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
    "The game visits every place by itself behind a loading screen (about 30-40 minutes, best docked), then "
    "restarts at the title screen, ready. Your saves are not touched.",
    "Play now",
    "You can prepare the graphics later: hold Minus (-) in the game for the settings menu, Switch tab.",

    "SwitchWakerHD: new in this version",
    "Prepare graphics: the console can now prepare the graphics of the whole game at once, so that places you have "
    "not visited yet are not black or late the first time.",
    "The game visits every place by itself behind a loading screen (about 30-40 minutes, best docked), then "
    "restarts at the title screen, ready. Your saves are not touched. What is already prepared on this console goes "
    "faster.",
    "Later",
    "This message does not show again. Prepare graphics stays in the settings menu: hold Minus (-) in the game, "
    "Switch tab.",

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

    "Prepare graphics",
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
};

const Texts kSpanish = {
    "SwitchWakerHD: primer arranque",
    "Los gráficos aún no se han preparado en esta consola.",
    "En este primer arranque algunas texturas pueden verse negras y algunos objetos pueden aparecer con un momento "
    "de retraso: la consola prepara cada efecto gráfico la primera vez que se dibuja. Mejora a medida que juegas, y "
    "lo preparado se guarda para los siguientes arranques.",
    "Preparar los gráficos ahora",
    "El juego visita cada lugar por sí solo detrás de una pantalla de carga (unos 30-40 minutos, mejor con la "
    "consola en la base) y después vuelve a la pantalla de título, listo. Tus partidas no se tocan.",
    "Jugar ahora",
    "Puedes preparar los gráficos más tarde: mantén pulsado Menos (-) en el juego para abrir los ajustes, pestaña "
    "Switch.",

    "SwitchWakerHD: novedad en esta versión",
    "Preparar gráficos: la consola ya puede preparar de una vez los gráficos de todo el juego, para que los lugares "
    "que aún no has visitado no se vean negros ni tarden la primera vez.",
    "El juego visita cada lugar por sí solo detrás de una pantalla de carga (unos 30-40 minutos, mejor con la "
    "consola en la base) y después vuelve a la pantalla de título, listo. Tus partidas no se tocan. Lo que esta "
    "consola ya tiene preparado va más rápido.",
    "Más tarde",
    "Este mensaje no volverá a aparecer. Preparar gráficos sigue en los ajustes: mantén pulsado Menos (-) en el "
    "juego, pestaña Switch.",

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

    "Preparar gráficos",
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
};

const Texts kFrench = {
    "SwitchWakerHD : premier démarrage",
    "Les graphismes n'ont pas encore été préparés sur cette console.",
    "Lors de ce premier démarrage, certaines textures peuvent apparaître noires et certains objets s'afficher avec "
    "un léger retard : la console prépare chaque effet graphique la première fois qu'il est dessiné. Cela s'améliore "
    "en jouant, et ce qui est préparé est conservé pour les démarrages suivants.",
    "Préparer les graphismes maintenant",
    "Le jeu visite chaque lieu tout seul derrière un écran de chargement (environ 30-40 minutes, de préférence sur la "
    "station d'accueil), puis revient à l'écran titre, prêt. Vos sauvegardes ne sont pas touchées.",
    "Jouer maintenant",
    "Vous pourrez préparer les graphismes plus tard : maintenez Moins (-) en jeu pour ouvrir les réglages, onglet "
    "Switch.",

    "SwitchWakerHD : nouveau dans cette version",
    "Préparer les graphismes : la console peut maintenant préparer en une fois les graphismes de tout le jeu, pour "
    "que les lieux pas encore visités ne soient ni noirs ni en retard la première fois.",
    "Le jeu visite chaque lieu tout seul derrière un écran de chargement (environ 30-40 minutes, de préférence sur la "
    "station d'accueil), puis revient à l'écran titre, prêt. Vos sauvegardes ne sont pas touchées. Ce qui est déjà "
    "préparé sur cette console va plus vite.",
    "Plus tard",
    "Ce message ne s'affichera plus. Préparer les graphismes reste dans les réglages : maintenez Moins (-) en jeu, "
    "onglet Switch.",

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

    "Préparer les graphismes",
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
};

const Texts kGerman = {
    "SwitchWakerHD: erster Start",
    "Die Grafik wurde auf dieser Konsole noch nicht vorbereitet.",
    "Beim ersten Start können manche Texturen schwarz aussehen und manche Objekte etwas verzögert erscheinen: Die "
    "Konsole bereitet jeden Grafikeffekt vor, wenn er zum ersten Mal gezeichnet wird. Das wird beim Spielen besser, "
    "und Vorbereitetes bleibt für die nächsten Starts erhalten.",
    "Grafik jetzt vorbereiten",
    "Das Spiel besucht hinter einem Ladebildschirm jeden Ort von selbst (etwa 30-40 Minuten, am besten in der "
    "Station) und startet dann fertig im Titelbildschirm neu. Deine Spielstände werden nicht verändert.",
    "Jetzt spielen",
    "Du kannst die Grafik später vorbereiten: Halte im Spiel Minus (-) für das Einstellungsmenü, Reiter Switch.",

    "SwitchWakerHD: neu in dieser Version",
    "Grafik vorbereiten: Die Konsole kann jetzt die Grafik des ganzen Spiels auf einmal vorbereiten, damit noch nicht "
    "besuchte Orte beim ersten Mal weder schwarz noch verzögert sind.",
    "Das Spiel besucht hinter einem Ladebildschirm jeden Ort von selbst (etwa 30-40 Minuten, am besten in der "
    "Station) und startet dann fertig im Titelbildschirm neu. Deine Spielstände werden nicht verändert. Was auf "
    "dieser Konsole schon vorbereitet ist, geht schneller.",
    "Später",
    "Diese Meldung erscheint nicht wieder. Grafik vorbereiten bleibt im Einstellungsmenü: Halte im Spiel Minus (-), "
    "Reiter Switch.",

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

    "Grafik vorbereiten",
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
};

const Texts kItalian = {
    "SwitchWakerHD: primo avvio",
    "La grafica non è ancora stata preparata su questa console.",
    "In questo primo avvio alcune texture potrebbero apparire nere e alcuni oggetti comparire con un attimo di "
    "ritardo: la console prepara ogni effetto grafico la prima volta che viene disegnato. Migliora man mano che "
    "giochi, e ciò che è pronto viene conservato per gli avvii successivi.",
    "Prepara la grafica ora",
    "Il gioco visita da solo ogni luogo dietro una schermata di caricamento (circa 30-40 minuti, meglio con la "
    "console nella base) e poi riparte dalla schermata del titolo, pronto. I tuoi salvataggi non vengono toccati.",
    "Gioca ora",
    "Puoi preparare la grafica più tardi: tieni premuto Meno (-) nel gioco per il menu delle impostazioni, scheda "
    "Switch.",

    "SwitchWakerHD: novità in questa versione",
    "Prepara la grafica: ora la console può preparare in una volta la grafica di tutto il gioco, così i luoghi non "
    "ancora visitati non saranno neri né in ritardo la prima volta.",
    "Il gioco visita da solo ogni luogo dietro una schermata di caricamento (circa 30-40 minuti, meglio con la "
    "console nella base) e poi riparte dalla schermata del titolo, pronto. I tuoi salvataggi non vengono toccati. "
    "Ciò che è già pronto su questa console va più veloce.",
    "Più tardi",
    "Questo messaggio non apparirà più. Prepara la grafica resta nelle impostazioni: tieni premuto Meno (-) nel "
    "gioco, scheda Switch.",

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

    "Prepara la grafica",
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
};

// the game's "language" setting (Wii U codes, hle/coreinit_misc.cpp), else the console's language
const Texts* choose() {
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
    switch (code) {
    case 2: return &kFrench;
    case 3: return &kGerman;
    case 4: return &kItalian;
    case 5: return &kSpanish;
    default: return &kEnglish;
    }
}

}  // namespace

const Texts& tx() {
    static const Texts* t = choose();
    return *t;
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
