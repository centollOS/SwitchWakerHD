using System.Collections.Generic;

namespace SwitchWakerHD.Installer;

// The Linux wording (most of it from PR #16): these keys win over shared/Strings.cs and DesktopStrings.cs' common ones.
static class LinuxStrings
{
    public static readonly Dictionary<string, (string es, string en)> Table = new()
    {
        // Welcome
        ["welcome_li1"]    = ("Python 3.14, solo para el instalador, en su carpeta de trabajo (el Python del sistema no se toca)",
                               "Python 3.14, for the installer only, in its working folder (the system's Python is left alone)"),
        ["welcome_li2"]    = ("devkitPro, las herramientas de Nintendo Switch (en /opt/devkitpro), o Docker/Podman si ya los tienes",
                               "devkitPro, the Nintendo Switch tools (in /opt/devkitpro), or Docker/Podman if you have them"),
        ["welcome_admin"]  = ("Si hay que instalar algo en el sistema, te pedirá tu contraseña.", "If something has to be installed on the system, it asks for your password."),

        // Toolchain
        ["lx_tc_native"]   = ("devkitPro encontrado en {0}; se compila sin contenedor.", "devkitPro found in {0}; building without a container."),
        ["lx_tc_container"]= ("Se compila con {0} (imagen devkitpro/devkita64; la primera vez la descarga, ~1,5 GB).",
                               "Building with {0} (the devkitpro/devkita64 image; downloaded the first time, ~1.5 GB)."),
        ["lx_tc_pkgs"]     = ("A devkitPro le faltan paquetes: {0}",                 "devkitPro is missing packages: {0}"),
        ["lx_tc_hosttools"]= ("Faltan herramientas del sistema: {0}",                "System tools are missing: {0}"),
        ["lx_tc_docker_denied"] = ("{0} está instalado pero tu usuario no puede usarlo (en Docker, añádelo al grupo docker y vuelve a iniciar sesión).",
                               "{0} is installed but your user cannot use it (for Docker, add it to the docker group and log in again)."),
        ["lx_tc_what_dkp"] = ("devkitPro y sus paquetes de Switch",                  "devkitPro and its Switch packages"),
        ["lx_tc_what_podman"] = ("Podman (para compilar en el contenedor de devkitPro)", "Podman (to build in devkitPro's container)"),
        ["lx_tc_unknown"]  = ("No se reconoce tu distribución. Instala Docker o Podman con tu gestor de paquetes y pulsa Continuar.",
                               "Your distribution is not recognised. Install Docker or Podman with your package manager and press Continue."),
        ["lx_tc_not_found"]= ("Sigue sin haber con qué compilar: ni devkitPro con sus paquetes ni Docker/Podman utilizables. Mira los detalles.",
                               "There is still nothing to build with: neither devkitPro with its packages nor a usable Docker/Podman. See the details."),
        ["lx_fwd_container"] = ("El icono del menú HOME solo se crea con devkitPro instalado (no en el contenedor): esta vez se construye el juego sin él.",
                               "The HOME-screen icon is only built with devkitPro installed (not in the container): this time the game is built without it."),
        ["lx_root_h"]      = ("Hace falta tu contraseña",                            "Your password is needed"),
        ["lx_root_body"]   = ("Para instalar {0} hay que ejecutar un paso como administrador. Abre una terminal (Ctrl+Alt+T), pega este comando, escribe tu contraseña y, cuando termine, pulsa Continuar.",
                               "Installing {0} needs one step as administrator. Open a terminal (Ctrl+Alt+T), paste this command, type your password and, when it finishes, press Continue."),
        ["lx_root_pkexec"] = ("Instalando {0}: escribe tu contraseña en la ventana que se abre...", "Installing {0}: type your password in the window that opens..."),
        ["lx_root_direct"] = ("Instalando {0} (el instalador ya es administrador)...", "Installing {0} (the installer already runs as administrator)..."),
        ["lx_root_failed"] = ("El paso de administrador no terminó (código {0}).",   "The administrator step did not finish (code {0})."),
        ["btn_copy_cmd"]   = ("Copiar comando",                                      "Copy command"),
        ["btn_done_continue"] = ("Ya está, continuar",                               "Done, continue"),
        ["dkp_found_at"]   = ("devkitPro encontrado: {0}",                           "devkitPro found: {0}"),
        ["dkp_why_ssl"]    = ("La conexión segura falló (\"certificate has expired\"): comprueba que la fecha y la hora del sistema son correctas y vuelve a intentarlo.",
                               "The secure connection failed (\"certificate has expired\"): check that the system's date and time are right and try again."),
        ["dkp_why_lock"]   = ("Otro proceso de pacman tiene bloqueada la base de datos. Espera a que termine y vuelve a intentarlo.",
                               "Another pacman process holds its database lock. Wait for it to finish and try again."),
        ["dkp_why_sig"]    = ("pacman no pudo verificar la firma de los paquetes. Pulsa Reintentar; si vuelve a fallar, el registro tiene el error de pacman.",
                               "pacman could not verify the packages' signatures. Press Retry; if it fails again, the log has pacman's error."),

        // Before anything
        ["pre_clock"]      = ("La fecha y hora del sistema ({0}) no coinciden con las de internet ({1}). Corrígelas (activa la hora automática): con un reloj desajustado las descargas fallan con \"certificate has expired\".",
                               "The system's date and time ({0}) do not match the internet's ({1}). Fix them (turn on automatic time): with a wrong clock the downloads fail with \"certificate has expired\"."),
        ["pre_tls"]        = ("No se pudo establecer una conexión segura con GitHub ({0}). Comprueba la fecha y hora del sistema, y que ningún proxy intercepta las conexiones HTTPS.",
                               "Could not make a secure connection to GitHub ({0}). Check the system's date and time, and that no proxy intercepts HTTPS connections."),
        ["key_txt"]        = ("{0} termina en .txt: cámbiale el nombre a {1}.",       "{0} ends in .txt: rename it to {1}."),

        // Failures
        ["err_io"]         = ("Error de archivos en el paso \"{0}\": {1}\nComprueba que hay espacio libre en el disco y que tu usuario puede leer la carpeta del juego; luego pulsa Reintentar.",
                               "File error in the step \"{0}\": {1}\nCheck that the disk has free space and that your user can read the game's folder; then press Retry."),
        ["hint_memory"]    = ("El equipo se quedó sin memoria al compilar. Cierra otros programas y pulsa Reintentar.", "The computer ran out of memory while compiling. Close other programs and press Retry."),
        ["hint_tool_missing"] = ("Falta una herramienta (de devkitPro o del sistema). Pulsa Reintentar: el instalador vuelve a comprobarlas.",
                               "A tool is missing (devkitPro's or the system's). Press Retry: the installer checks them again."),
        ["hint_av"]        = ("Se denegó el acceso a un archivo: comprueba que tu usuario puede leer la carpeta del juego y escribir en ~/SwitchWakerHD-Build, y pulsa Reintentar.",
                               "Access to a file was denied: check that your user can read the game's folder and write to ~/SwitchWakerHD-Build, then press Retry."),
        ["hint_silent"]    = ("El build se detuvo sin mensaje: una herramienta no llegó a arrancar. Pulsa Reintentar; si vuelve a pasar, comparte el registro (installer.log) en una issue.",
                               "The build stopped without a message: a tool did not start. Press Retry; if it happens again, share the log (installer.log) in an issue."),

        // Done page
        ["sd_hint"]        = ("Pon la microSD en un lector de tarjetas, o en la Switch abre hekate > Tools > USB Tools > SD Card. Si no aparece, ábrela en el gestor de archivos para montarla.",
                               "Put the microSD card in a card reader, or on the Switch open hekate > Tools > USB Tools > SD Card. If it does not show up, open it in the file manager to mount it."),
        ["sd_fat32"]       = ("{0} está en FAT32, que no admite archivos de más de 4 GB. Formatea la tarjeta en exFAT (con hekate) o cópiala por USB con DBI.",
                               "{0} is FAT32, which cannot hold files over 4 GB. Format the card as exFAT (with hekate) or copy over USB with DBI."),
    };
}
