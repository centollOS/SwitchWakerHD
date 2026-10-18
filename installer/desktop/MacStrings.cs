using System.Collections.Generic;

namespace SwitchWakerHD.Installer;

// The macOS wording: these keys win over shared/Strings.cs (Str.Override), the rest is the Windows installer's.
static class MacStrings
{
    public static readonly Dictionary<string, (string es, string en)> Table = new()
    {
        // Welcome
        ["welcome_body"]   = ("Este asistente prepara SwitchWakerHD a partir de tu propia copia del juego. Se encarga de todo: instala lo que haga falta, construye el juego para la Switch y lo copia a tu tarjeta SD. No hace falta usar el Terminal.",
                               "This wizard builds SwitchWakerHD from your own copy of the game. It takes care of everything: installs what is needed, builds the game for the Switch and copies it to your SD card. No Terminal needed."),
        ["welcome_li1"]    = ("Python 3.14, solo para el instalador, en su carpeta de trabajo (el Python del Mac no se toca)",
                               "Python 3.14, for the installer only, in its working folder (the Mac's own Python is left alone)"),
        ["welcome_li2"]    = ("devkitPro, las herramientas de Nintendo Switch (en /opt/devkitpro), y las herramientas de línea de comandos de Apple si faltan",
                               "devkitPro, the Nintendo Switch tools (in /opt/devkitpro), and Apple's command line tools if they are missing"),
        ["welcome_admin"]  = ("macOS te pedirá tu contraseña una vez, para instalar devkitPro. La primera vez puede pedirte también permiso para leer la carpeta donde está el juego: acéptalo.",
                               "macOS asks for your password once, to install devkitPro. The first time it may also ask to let the installer read the folder with the game: allow it."),

        // Game page
        ["dlg_folder"]     = ("Selecciona la carpeta del juego (contiene code, content y meta)", "Select the game folder (contains code, content and meta)"),

        // Progress
        ["prog_body"]      = ("Puedes dejar el Mac trabajando (enchufado, para que no se duerma). El build tarda entre 10 y 30 minutos.",
                               "You can leave the Mac working (plugged in, so it does not sleep). The build takes 10 to 30 minutes."),
        ["step_python"]    = ("Python 3.14",                                          "Python 3.14"),
        ["step_devkit"]    = ("Herramientas de Apple y devkitPro",                    "Apple's tools and devkitPro"),

        // Python
        ["py_mac_arch"]    = ("Este Mac ({0}) no es Apple Silicon ni Intel de 64 bits.", "This Mac ({0}) is neither Apple Silicon nor 64-bit Intel."),
        ["py_mac_downloading"] = ("Descargando Python {0}...",                        "Downloading Python {0}..."),
        ["py_mac_unpacking"] = ("Preparando Python...",                               "Setting up Python..."),
        ["py_mac_fail"]    = ("Python no arranca desde {0}. Borra esa carpeta y pulsa Reintentar.", "Python does not start from {0}. Delete that folder and press Retry."),

        // Apple's Command Line Tools
        ["clt_checking"]   = ("Buscando las herramientas de línea de comandos de Apple...", "Looking for Apple's command line tools..."),
        ["clt_ok"]         = ("Herramientas de línea de comandos de Apple listas.",   "Apple's command line tools ready."),
        ["clt_notice_h"]   = ("Hace falta tu ayuda: herramientas de Apple",           "Your help is needed: Apple's tools"),
        ["clt_notice"]     = ("macOS ha abierto una ventana para instalar las herramientas de línea de comandos (make, git y el compilador).\n1. Pulsa Instalar y acepta la licencia.\n2. Espera a que termine (unos minutos).\n3. Pulsa Continuar aquí.\nSi no ves la ventana, búscala en el Dock.",
                               "macOS opened a window to install the command line tools (make, git and the compiler).\n1. Press Install and accept the license.\n2. Wait for it to finish (a few minutes).\n3. Press Continue here.\nIf you don't see the window, look for it in the Dock."),
        ["clt_missing"]    = ("Las herramientas de Apple todavía no están instaladas.", "Apple's tools are not installed yet."),

        // devkitPro
        ["dkp_found_at"]   = ("devkitPro y los paquetes de Switch ya están en {0}.",  "devkitPro and the Switch packages are already in {0}."),
        ["dkp_mac_password"] = ("Instalando devkitPro y los paquetes de Switch: macOS te pide tu contraseña...",
                               "Installing devkitPro and the Switch packages: macOS asks for your password..."),
        ["dkp_mac_prompt"] = ("El instalador de SwitchWakerHD quiere instalar devkitPro (las herramientas de Nintendo Switch) en /opt/devkitpro.",
                               "The SwitchWakerHD installer wants to install devkitPro (the Nintendo Switch tools) in /opt/devkitpro."),
        ["dkp_mac_cancelled"] = ("Se canceló la ventana de la contraseña: sin ella no se puede instalar devkitPro. Pulsa Reintentar y escribe la contraseña de tu usuario del Mac.",
                               "The password window was cancelled: devkitPro cannot be installed without it. Press Retry and type your Mac user's password."),
        ["dkp_mac_fail"]   = ("No se pudo pedir la contraseña para instalar devkitPro ({0}). Pulsa Reintentar; el registro tiene los detalles.",
                               "Could not ask for the password to install devkitPro ({0}). Press Retry; the log has the details."),
        ["dkp_why_ssl"]    = ("La conexión segura falló (\"certificate has expired\"): comprueba que la fecha y la hora del Mac son correctas (Ajustes del Sistema > General > Fecha y hora) y vuelve a intentarlo.",
                               "The secure connection failed (\"certificate has expired\"): check that the Mac's date and time are right (System Settings > General > Date & Time) and try again."),
        ["dkp_why_lock"]   = ("Otro proceso de pacman tiene bloqueada la base de datos. Espera a que termine (o reinicia el Mac) y vuelve a intentarlo.",
                               "Another pacman process holds its database lock. Wait for it to finish (or restart the Mac) and try again."),
        ["dkp_why_sig"]    = ("pacman no pudo verificar la firma de los paquetes. Pulsa Reintentar; si vuelve a fallar, el registro tiene el error de pacman.",
                               "pacman could not verify the packages' signatures. Press Retry; if it fails again, the log has pacman's error."),

        // Before anything
        ["pre_clock"]      = ("La fecha y hora del Mac ({0}) no coinciden con las de internet ({1}). Corrígelas (Ajustes del Sistema > General > Fecha y hora): con un reloj desajustado las descargas fallan con \"certificate has expired\".",
                               "The Mac's date and time ({0}) do not match the internet's ({1}). Fix them (System Settings > General > Date & Time): with a wrong clock the downloads fail with \"certificate has expired\"."),
        ["pre_tls"]        = ("No se pudo establecer una conexión segura con GitHub ({0}). Comprueba la fecha y hora del Mac, y que ningún proxy o filtro intercepta las conexiones HTTPS.",
                               "Could not make a secure connection to GitHub ({0}). Check the Mac's date and time, and that no proxy or filter intercepts HTTPS connections."),
        ["key_txt"]        = ("{0} termina en .txt. En el Finder, selecciónalo, Archivo > Obtener información, y en Nombre y extensión déjalo como {1}.",
                               "{0} ends in .txt. In the Finder, select it, File > Get Info, and under Name & Extension make it {1}."),
        ["rel_no_script"]  = ("La versión descargada no contiene tools/switch/make_sd.py.", "The downloaded release has no tools/switch/make_sd.py."),

        // Failures
        ["err_io"]         = ("Error de archivos en el paso \"{0}\": {1}\nComprueba que hay espacio libre en el disco y que macOS deja al instalador leer la carpeta del juego (Ajustes del Sistema > Privacidad y seguridad > Archivos y carpetas); luego pulsa Reintentar.",
                               "File error in the step \"{0}\": {1}\nCheck that the disk has free space and that macOS lets the installer read the game's folder (System Settings > Privacy & Security > Files and Folders); then press Retry."),
        ["hint_memory"]    = ("El Mac se quedó sin memoria al compilar. Cierra otros programas y pulsa Reintentar.", "The Mac ran out of memory while compiling. Close other programs and press Retry."),
        ["hint_tool_missing"] = ("Falta una herramienta (de devkitPro o de Apple). Pulsa Reintentar: el instalador vuelve a comprobarlas.",
                               "A tool is missing (devkitPro's or Apple's). Press Retry: the installer checks them again."),
        ["hint_av"]        = ("macOS denegó el acceso a un archivo. Si el juego está en Escritorio, Documentos, Descargas o un disco externo, permite el acceso al instalador en Ajustes del Sistema > Privacidad y seguridad > Archivos y carpetas (o Acceso total al disco), y pulsa Reintentar.",
                               "macOS denied access to a file. If the game is on the Desktop, in Documents, Downloads or on an external disk, allow the installer in System Settings > Privacy & Security > Files and Folders (or Full Disk Access), then press Retry."),
        ["hint_silent"]    = ("El build se detuvo sin mensaje: una herramienta no llegó a arrancar. Pulsa Reintentar; si vuelve a pasar, comparte el registro (installer.log) en una issue.",
                               "The build stopped without a message: a tool did not start. Press Retry; if it happens again, share the log (installer.log) in an issue."),
        ["mm_rvz_note"]    = ("Un .rvz de Dolphin se lee con Python 3.14, que es el que usa este instalador.",
                               "A Dolphin .rvz is read with Python 3.14, the one this installer uses."),

        // Done page
        ["xfer_sd"]        = ("Con la microSD en el Mac",                             "With the microSD card in the Mac"),
        ["sd_hint"]        = ("Pon la microSD en un lector de tarjetas, o en la Switch abre hekate > Tools > USB Tools > SD Card y conecta el cable USB: la tarjeta aparece en el Finder.",
                               "Put the microSD card in a card reader, or on the Switch open hekate > Tools > USB Tools > SD Card and plug in the USB cable: the card shows up in the Finder."),
        ["sd_mac_todo"]    = ("{0} archivos que copiar ({1} ya estaban en la tarjeta).", "{0} files to copy ({1} were already on the card)."),
        ["sd_fail_mac"]    = ("La copia falló ({0}): comprueba que la tarjeta sigue conectada y no está protegida contra escritura, y vuelve a copiar; o cópiala a mano.",
                               "The copy failed ({0}): check that the card is still connected and not write-protected, and copy again; or copy it by hand."),
        ["sd_copied"]      = ("Copiado a {0}. Expulsa la tarjeta antes de sacarla.",  "Copied to {0}. Eject the card before removing it."),
        ["sd_ejected"]     = ("Tarjeta expulsada: ya puedes sacarla (o salir de hekate).", "Card ejected: you can remove it now (or leave hekate)."),
        ["sd_eject_fail"]  = ("No se pudo expulsar {0}: ciérrala en el Finder y expúlsala desde allí.", "Could not eject {0}: close it in the Finder and eject it there."),
        ["sd_fat32"]       = ("{0} está en FAT32, que no admite archivos de más de 4 GB. Formatea la tarjeta en exFAT (con hekate o el formateador oficial de la SD Association).",
                               "{0} is FAT32, which cannot hold files over 4 GB. Format the card as exFAT (with hekate or the SD Association's official formatter)."),
        ["manual_body"]    = ("Copia el contenido de esta carpeta (la carpeta switch) a la raíz de la microSD y reemplaza los archivos si pregunta. El Finder deja archivos ._ en la tarjeta; la opción \"Con la microSD en el Mac\" no.",
                               "Copy the contents of this folder (the switch folder) to the root of the microSD card, replacing files if asked. The Finder leaves ._ files on the card; the \"microSD card in the Mac\" option does not."),
        ["btn_eject"]      = ("Expulsar tarjeta",                                     "Eject card"),
        ["btn_open_out"]   = ("Abrir en el Finder",                                   "Show in Finder"),
        ["btn_yes_cancel"] = ("Sí, cancelar",                                         "Yes, cancel"),
        ["btn_no"]         = ("No",                                                   "No"),
    };
}
