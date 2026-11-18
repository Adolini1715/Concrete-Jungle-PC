/*
 * Predator: Concrete Jungle PC launcher (launcher.exe, next to pcj.exe).
 *
 * Play, Update (latest GitHub release, rebuilt from the player's own disc by the installer),
 * Settings (pcj.ini, the same keys as the game's Options > PC Settings page), Texture packs
 * (on/off: an off pack moves to textures\disabled, which the game skips) and Report a problem
 * (the latest session's logs zipped to the Desktop, the GitHub issue page opened; the logs
 * never contain the Windows user name, src/async_io.c).
 *
 * Built by the installer from source like pcj.exe: SDL2 for the window, stb_truetype with a
 * Windows font (Bahnschrift, else Segoe UI, else Arial; CJ_LAUNCHER_FONT=<ttf> overrides).
 * Nothing from the game is in it.
 *
 * The install folder (the one with Install.bat) is found from install-root.txt next to the
 * launcher, which the installer writes, else the folder above it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <SDL2/SDL.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#define SEP "\\"
#else
/* Linux build of the launcher (Steam Deck / desktop Linux): Play runs PCJ-Linux.sh / pcj-linux next to
 * it, folders and the issue page open with xdg-open, updates come as a new build folder. */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <time.h>
#define SEP "/"
#define _stricmp strcasecmp
static void open_with_system(const char *what)
{
    pid_t p = fork();
    if (p == 0) { setsid(); execlp("xdg-open", "xdg-open", what, (char *)NULL); _exit(127); }
}
#endif

#define W 960
#define H 540
#define ISSUES_URL "https://github.com/Adolini1715/Concrete-Jungle-PC/issues/new"

/* ---------------------------------------------------------------- paths */
static char s_dir[1024], s_repo[1024], s_ini[1100], s_tex[1100];

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void find_paths(void)
{
    char *b = SDL_GetBasePath(), p[1200];
    FILE *f;
    snprintf(s_dir, sizeof s_dir, "%s", b ? b : "." SEP);
    SDL_free(b);
    if (s_dir[strlen(s_dir) - 1] == '\\' || s_dir[strlen(s_dir) - 1] == '/') s_dir[strlen(s_dir) - 1] = 0;
    snprintf(s_ini, sizeof s_ini, "%s" SEP "pcj.ini", s_dir);
    snprintf(s_tex, sizeof s_tex, "%s" SEP "textures", s_dir);
    s_repo[0] = 0;
    snprintf(p, sizeof p, "%s" SEP "install-root.txt", s_dir);
    if ((f = fopen(p, "r"))) {
        if (fgets(s_repo, sizeof s_repo, f)) s_repo[strcspn(s_repo, "\r\n")] = 0;
        fclose(f);
    }
    snprintf(p, sizeof p, "%s" SEP "installer" SEP "update.py", s_repo);
    if (!s_repo[0] || !file_exists(p)) {   /* the game folder sits inside the install folder */
        char *q;
        snprintf(s_repo, sizeof s_repo, "%s", s_dir);
        if ((q = strrchr(s_repo, SEP[0]))) *q = 0;
        snprintf(p, sizeof p, "%s" SEP "installer" SEP "update.py", s_repo);
        if (!file_exists(p)) s_repo[0] = 0;
    }
}

/* ---------------------------------------------------------------- pcj.ini */
typedef struct { const char *key, *label, *help; int lo, hi, step, def, exp; } Setting;   /* exp: on the Experimental page */
static const Setting S[] = {
    {"CJ_FPS", "Frame rate", "60 fps, or 30 like the original.", 30, 60, 30, 30},
    {"CJ_GL_SCALE", "Resolution scale", "1 = 640x480 (original) ... 3 = 1440p ... 5 = 4K.", 1, 5, 1, 1},
    {"CJ_WIDESCREEN", "Widescreen", "The game's own 16:9 mode.", 0, 1, 1, 1},
    {"CJ_FULLSCREEN", "Fullscreen", "Desktop fullscreen. F11 or Alt+Enter in game too.", 0, 1, 1, 0},
    {"CJ_VSYNC", "V-Sync", "Sync to the monitor. Off has the lowest input lag.", 0, 1, 1, 0},
    {"CJ_TEX_REPLACE", "Texture packs", "Use the HD packs in the textures folder.", 0, 1, 1, 1},
    {"CJ_MOUSE", "Mouse look", "Turn the camera with the mouse.", 0, 1, 1, 0},
    {"CJ_MOUSE_INVERT", "Invert mouse", "Mouse up looks down.", 0, 1, 1, 0},
    {"CJ_CONTROLLER_ONLY", "Controller only", "Keyboard and mouse don't control the game (no stray shots while taking screenshots).", 0, 1, 1, 0},
    {"CJ_CAM_MODERN", "Modern camera", "The camera stops when you let go of the stick.", 0, 1, 1, 1},
    {"CJ_GLOW_TRAILS", "Screen trails", "The original glow and blur lag behind the camera, leaving a ghost when it moves. Off removes the lag.", 0, 1, 1, 1},
    {"CJ_LANG", "Language", "Text language. Portuguese reads the translation from the lang\\pt-br folder.", 0, 1, 1, 0},
    {"CJ_GLYPHS", "PC button prompts", "Keyboard or modern controller icons in game.", 0, 1, 1, 1},
    {"CJ_GL_SHADER_CACHE", "Shader cache", "Keeps compiled shaders so each one only stutters once.", 0, 1, 1, 1},
    {"CJ_SAFE_RENDER", "Compatibility mode", "Turns off this version's renderer speedups. Try it if anything looks wrong.", 0, 1, 1, 0, 1},
    {"CJ_API_TAKEOVER", "Native renderer", "May run faster. Draws from the game's own calls. Turn off if anything looks wrong.", 0, 1, 1, 0, 1},
    {"CJ_BOSS_POISE", "Boss poise", "Bosses stop flinching for a few seconds after a quick run of hits, so they can't be stun-locked.", 0, 1, 1, 0, 1},
    {"CJ_TEST_CAPTURE", "Test captures", "Records 3 short frame captures per session for bug reports.", 0, 1, 1, 0, 1},
};
#define NS ((int)(sizeof S / sizeof S[0]))
static int s_val[NS], s_ini_dirty;

/* ---------------------------------------------------------------- launcher language
 * The launcher follows the Language setting (CJ_LANG): in Portuguese every string drawn through
 * text() / text_w() / msg() is looked up here by its English text. Written in UTF-8, converted to
 * Latin-1 (what the baked font covers) on first use. */
static const char *const k_pt[][2] = {
    {"Play", "Jogar"}, {"Quit", "Sair"}, {"Back", "Voltar"}, {"Select", "Selecionar"}, {"Change", "Alterar"},
    {"On", "Ligado"}, {"Off", "Desligado"}, {"English", "Inglês"},
    {"PC Settings", "Configurações do PC"}, {"Texture Packs", "Pacotes de texturas"}, {"Check for Updates", "Procurar atualizações"},
    {"Performance Tests", "Testes de desempenho"}, {"Report a Problem", "Relatar um problema"}, {"Experimental", "Experimental"},
    {"PC SETTINGS", "CONFIGURAÇÕES DO PC"}, {"EXPERIMENTAL", "EXPERIMENTAL"}, {"TEXTURE PACKS", "PACOTES DE TEXTURAS"},
    {"PERFORMANCE TESTS", "TESTES DE DESEMPENHO"}, {"AVAILABLE TO DOWNLOAD", "DISPONÍVEIS PARA BAIXAR"}, {"VERSION", "VERSÃO"},
    {"Frame rate", "Taxa de quadros"}, {"60 fps, or 30 like the original.", "60 fps, ou 30 como no original."},
    {"Resolution scale", "Escala de resolução"}, {"1 = 640x480 (original) ... 3 = 1440p ... 5 = 4K.", "1 = 640x480 (original) ... 3 = 1440p ... 5 = 4K."},
    {"Widescreen", "Widescreen"}, {"The game's own 16:9 mode.", "O modo 16:9 do próprio jogo."},
    {"Fullscreen", "Tela cheia"}, {"Desktop fullscreen. F11 or Alt+Enter in game too.", "Tela cheia da área de trabalho. F11 ou Alt+Enter também no jogo."},
    {"V-Sync", "V-Sync"}, {"Sync to the monitor. Off has the lowest input lag.", "Sincroniza com o monitor. Desligado tem o menor atraso de comandos."},
    {"Texture packs", "Pacotes de texturas"}, {"Use the HD packs in the textures folder.", "Usa os pacotes HD da pasta textures."},
    {"Mouse look", "Câmera com mouse"}, {"Turn the camera with the mouse.", "Gira a câmera com o mouse."},
    {"Invert mouse", "Inverter mouse"}, {"Mouse up looks down.", "Mouse para cima olha para baixo."},
    {"Controller only", "Só controle"}, {"Keyboard and mouse don't control the game (no stray shots while taking screenshots).",
                                         "Teclado e mouse não controlam o jogo (sem tiros acidentais ao tirar capturas de tela)."},
    {"Modern camera", "Câmera moderna"}, {"The camera stops when you let go of the stick.", "A câmera para quando você solta o analógico."},
    {"Screen trails", "Rastro na tela"}, {"The original glow and blur lag behind the camera, leaving a ghost when it moves. Off removes the lag.",
                                          "O brilho e o desfoque originais ficam atrás da câmera e deixam um fantasma quando ela se move. Desligado remove o atraso."},
    {"Language", "Idioma"}, {"Text language. Portuguese reads the translation from the lang\\pt-br folder.", "Idioma dos textos. Português usa a tradução da pasta lang\\pt-br."},
    {"PC button prompts", "Botões de PC"}, {"Keyboard or modern controller icons in game.", "Ícones de teclado ou de controle moderno no jogo."},
    {"Shader cache", "Cache de shaders"}, {"Keeps compiled shaders so each one only stutters once.", "Guarda os shaders compilados para cada um travar só uma vez."},
    {"Compatibility mode", "Modo de compatibilidade"}, {"Turns off this version's renderer speedups. Try it if anything looks wrong.",
                                                         "Desliga as otimizações de renderização desta versão. Tente se algo parecer errado."},
    {"Native renderer", "Renderizador nativo"}, {"May run faster. Draws from the game's own calls. Turn off if anything looks wrong.",
                                                  "Pode rodar mais rápido. Desenha a partir das chamadas do próprio jogo. Desligue se algo parecer errado."},
    {"Boss poise", "Postura dos chefes"}, {"Bosses stop flinching for a few seconds after a quick run of hits, so they can't be stun-locked.",
                                           "Chefes param de recuar por alguns segundos após uma sequência rápida de golpes, para não ficarem presos atordoados."},
    {"Test captures", "Capturas de teste"}, {"Records 3 short frame captures per session for bug reports.", "Grava 3 capturas curtas por sessão para relatórios de erros."},
    {"Apply Steam Deck preset", "Aplicar predefinição do Steam Deck"}, {"Steam Deck preset applied.%s", "Predefinição do Steam Deck aplicada.%s"},
    {"60 fps, 2x, fullscreen, V-Sync, controller prompts, regular renderer.", "60 fps, 2x, tela cheia, V-Sync, botões de controle, renderizador normal."},
    {"Settings still being tested. Turn one off if anything looks wrong.", "Opções ainda em teste. Desligue uma se algo parecer errado."},
    {"Steam Deck detected: Deck settings applied.%s", "Steam Deck detectado: configurações do Deck aplicadas.%s"}, {" Change them in PC Settings.", " Altere-as em Configurações do PC."},
    {"Checking for updates", "Procurando atualizações"}, {"Up to date", "Atualizado"}, {"Update available: %s", "Atualização disponível: %s"},
    {"Could not reach GitHub", "Não foi possível acessar o GitHub"}, {"Linux test build: update by copying a new build in", "Versão de teste para Linux: atualize copiando uma nova versão"},
    {"Run Install.bat once to enable updates", "Execute o Install.bat uma vez para ativar as atualizações"},
    {"Update to %s", "Atualizar para %s"}, {"You have the latest version.%s", "Você tem a versão mais recente.%s"},
    {"A new version is ready: choose Update to %s", "Uma nova versão está pronta: escolha Atualizar para %s"},
    {"Could not reach GitHub.%s", "Não foi possível acessar o GitHub.%s"}, {" Check the internet connection.", " Verifique a conexão com a internet."},
    {"Linux test build: copy the new build's files into this folder%s", "Versão de teste para Linux: copie os arquivos da nova versão para esta pasta%s"},
    {"Updates need the install folder.%s", "As atualizações precisam da pasta de instalação.%s"}, {" Run Install.bat once.", " Execute o Install.bat uma vez."},
    {"Test 1: Regular renderer", "Teste 1: Renderizador normal"}, {"The regular renderer with the new buffer streaming.", "O renderizador normal com o novo envio de buffers."},
    {"Test 2: Regular, old buffers", "Teste 2: Normal, buffers antigos"}, {"The same, with the new buffer streaming off, to compare with test 1.", "O mesmo, com o novo envio de buffers desligado, para comparar com o teste 1."},
    {"Test 3: Native renderer", "Teste 3: Renderizador nativo"}, {"The experimental native renderer.", "O renderizador nativo experimental."},
    {"Same save and spot each time, about two minutes, then quit.", "Mesmo save e mesmo lugar toda vez, cerca de dois minutos, depois saia."},
    {"Start test", "Iniciar teste"}, {"Get  %s", "Baixar  %s"}, {"Get all of them", "Baixar todos"}, {"Open the textures folder", "Abrir a pasta textures"},
    {"Downloading %d%%", "Baixando %d%%"}, {"Failed: try again", "Falhou: tente de novo"}, {"Looking for packs to download...", "Procurando pacotes para baixar..."},
    {"No packs yet.", "Nenhum pacote ainda."}, {"Download", "Baixar"}, {"Switch on / off", "Ligar / desligar"},
    {"Drop a texture mod or pack on this window to add it.", "Solte um mod de texturas ou pacote nesta janela para adicioná-lo."},
    {"ADDING A MOD", "ADICIONANDO UM MOD"}, {"DONE: packs apply the next time you play", "PRONTO: os pacotes valem na próxima vez que você jogar"},
    {"COULDN'T ADD THAT", "NÃO FOI POSSÍVEL ADICIONAR"}, {"Still working on the last drop.", "Ainda processando o último arquivo."},
    {"Run Install.bat once to add mods.", "Execute o Install.bat uma vez para adicionar mods."},
    {"Too many files at once: drop fewer.", "Arquivos demais de uma vez: solte menos."}, {"Couldn't start the converter.", "Não foi possível iniciar o conversor."},
    {"Couldn't start the converter (run Install.bat once).", "Não foi possível iniciar o conversor (execute o Install.bat uma vez)."},
    {"The converter stopped without finishing.", "O conversor parou sem terminar."},
    {"Could not move %s (is the game running?)", "Não foi possível mover %s (o jogo está aberto?)"},
    {"Could not start pcj.exe%s", "Não foi possível iniciar o pcj.exe%s"}, {" (is it next to the launcher?)", " (ele está ao lado do launcher?)"},
    {"The install folder is missing%s", "A pasta de instalação não foi encontrada%s"}, {": download the latest release and run Install.bat", ": baixe a versão mais recente e execute o Install.bat"},
    {"Could not start %s", "Não foi possível iniciar %s"}, {"No game logs yet%s", "Ainda não há registros do jogo%s"}, {": play once, then report", ": jogue uma vez e depois relate"},
    {"Could not write %s", "Não foi possível gravar %s"}, {"Saved %s: attach it to the issue that just opened", "%s salvo: anexe-o ao relatório que acabou de abrir"},
};
static int s_ui_pt;   /* set from the Language setting (see val_str's caller) */
static void l1(const char *u, char *o, size_t n)   /* UTF-8 -> Latin-1 */
{
    const unsigned char *p = (const unsigned char *)u;
    size_t k = 0;
    while (*p && k + 1 < n) {
        if (*p < 0x80) o[k++] = (char)*p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { o[k++] = (char)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F)); p += 2; }
        else { o[k++] = '?'; p++; while ((*p & 0xC0) == 0x80) p++; }
    }
    o[k] = 0;
}
static const char *tr(const char *s)
{
    static char *conv[sizeof k_pt / sizeof k_pt[0]];
    size_t i;
    if (!s_ui_pt || !s) return s;
    for (i = 0; i < sizeof k_pt / sizeof k_pt[0]; i++)
        if (!strcmp(s, k_pt[i][0])) {
            if (!conv[i]) { size_t n = strlen(k_pt[i][1]) + 1; if ((conv[i] = malloc(n))) l1(k_pt[i][1], conv[i], n); }
            return conv[i] ? conv[i] : s;
        }
    return s;
}


static void ini_load(void)
{
    char line[1024];
    FILE *f;
    int i;
    for (i = 0; i < NS; i++) s_val[i] = S[i].def;
    if (!(f = fopen(s_ini, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == ';' || line[0] == '#' || !eq) continue;
        *eq = 0;
        for (i = 0; i < NS; i++)
            if (!strcmp(line, S[i].key)) {
                int v = atoi(eq + 1);
                if (v < S[i].lo) v = S[i].lo;
                if (v > S[i].hi) v = S[i].hi;
                s_val[i] = v;
            }
    }
    fclose(f);
}

/* rewrite only our keys, keep every other line (and the CRLF style) as it was */
static void ini_save(void)
{
    char *buf = NULL, *out, *p;
    size_t n = 0, cap;
    int i, done[NS] = {0}, crlf = 1;
    FILE *f = fopen(s_ini, "rb");
    char tmp[1200];
    if (f) {
        fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc(n + 1);
        n = fread(buf, 1, n, f);
        buf[n] = 0;
        fclose(f);
        crlf = n == 0 || strstr(buf, "\r\n") != NULL;
    } else buf = calloc(1, 1);
    cap = n + 64 * NS + 256;
    out = malloc(cap);
    out[0] = 0;
    for (p = buf; *p;) {
        char *e = strchr(p, '\n'), line[1024];
        size_t l = e ? (size_t)(e - p + 1) : strlen(p);
        int hit = -1;
        snprintf(line, sizeof line, "%.*s", (int)l, p);
        for (i = 0; i < NS; i++) {
            size_t kl = strlen(S[i].key);
            if (!strncmp(line, S[i].key, kl) && line[kl] == '=') hit = i;
        }
        if (hit >= 0) {
            char nl[96];
            snprintf(nl, sizeof nl, "%s=%d%s", S[hit].key, s_val[hit], crlf ? "\r\n" : "\n");
            strcat(out, nl);
            done[hit] = 1;
        } else strcat(out, line);
        p += l;
    }
    for (i = 0; i < NS; i++)
        if (!done[i]) {
            char nl[96];
            snprintf(nl, sizeof nl, "%s=%d%s", S[i].key, s_val[i], crlf ? "\r\n" : "\n");
            strcat(out, nl);
        }
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ini);
    if ((f = fopen(tmp, "wb"))) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
#ifdef _WIN32
        MoveFileExA(tmp, s_ini, MOVEFILE_REPLACE_EXISTING);
#else
        rename(tmp, s_ini);
#endif
    }
    free(buf);
    free(out);
    s_ini_dirty = 0;
}

/* ---------------------------------------------------------------- texture packs */
typedef struct { char name[260]; int on; long long size; int dir; } Pack;
static Pack s_packs[64];
static int s_npack;

#ifdef _WIN32
static long long dir_size(const char *d)
{
    char pat[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    long long t = 0;
    snprintf(pat, sizeof pat, "%s\\*", d);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        char sub[1200];
        if (fd.cFileName[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s\\%s", d, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += dir_size(sub);
        else t += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return t;
}

static void scan_dir(const char *d, int on)
{
    char pat[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", d);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        const char *n = fd.cFileName;
        size_t l = strlen(n);
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        Pack *p;
        if (n[0] == '.' || s_npack >= 64) continue;
        if (on && (!_stricmp(n, "dump") || !_stricmp(n, "glyphs") || !_stricmp(n, "disabled"))) continue;
        if (!isdir && !(l > 4 && !_stricmp(n + l - 4, ".zip"))) continue;
        p = &s_packs[s_npack++];
        snprintf(p->name, sizeof p->name, "%s", n);
        p->on = on;
        p->dir = isdir;
        if (isdir) {
            char sub[1200];
            snprintf(sub, sizeof sub, "%s\\%s", d, n);
            p->size = dir_size(sub);
        } else p->size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static long long dir_size(const char *d)
{
    DIR *dp = opendir(d);
    struct dirent *de;
    long long t = 0;
    if (!dp) return 0;
    while ((de = readdir(dp))) {
        char sub[1200];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", d, de->d_name);
        if (stat(sub, &st)) continue;
        t += S_ISDIR(st.st_mode) ? dir_size(sub) : (long long)st.st_size;
    }
    closedir(dp);
    return t;
}

static void scan_dir(const char *d, int on)
{
    DIR *dp = opendir(d);
    struct dirent *de;
    if (!dp) return;
    while ((de = readdir(dp))) {
        const char *n = de->d_name;
        size_t l = strlen(n);
        char sub[1200];
        struct stat st;
        int isdir;
        Pack *p;
        if (n[0] == '.' || s_npack >= 64) continue;
        snprintf(sub, sizeof sub, "%s/%s", d, n);
        if (stat(sub, &st)) continue;
        isdir = S_ISDIR(st.st_mode);
        if (on && (!_stricmp(n, "dump") || !_stricmp(n, "glyphs") || !_stricmp(n, "disabled"))) continue;
        if (!isdir && !(l > 4 && !_stricmp(n + l - 4, ".zip"))) continue;
        p = &s_packs[s_npack++];
        snprintf(p->name, sizeof p->name, "%s", n);
        p->on = on;
        p->dir = isdir;
        p->size = isdir ? dir_size(sub) : (long long)st.st_size;
    }
    closedir(dp);
}
#endif

static int pack_cmp(const void *a, const void *b) { return strcmp(((const Pack *)a)->name, ((const Pack *)b)->name); }

static void packs_scan(void)
{
    char dis[1200];
    s_npack = 0;
#ifdef _WIN32
    CreateDirectoryA(s_tex, NULL);
    snprintf(dis, sizeof dis, "%s\\disabled", s_tex);
    scan_dir(s_tex, 1);
    scan_dir(dis, 0);
#else
    mkdir(s_tex, 0755);
    snprintf(dis, sizeof dis, "%s/disabled", s_tex);
    scan_dir(s_tex, 1);
    scan_dir(dis, 0);
#endif
    qsort(s_packs, (size_t)s_npack, sizeof *s_packs, pack_cmp);
}

static const char *s_msg;   /* status line */
static char s_msgbuf[512];
static Uint32 s_msg_t;

static void msg(const char *fmt, const char *a)
{
    snprintf(s_msgbuf, sizeof s_msgbuf, tr(fmt), a ? tr(a) : "");
    s_msg = s_msgbuf;
    s_msg_t = SDL_GetTicks();
}

static void pack_toggle(int i)
{
#ifdef _WIN32
    char from[1200], to[1200], dis[1200];
    Pack *p = &s_packs[i];
    snprintf(dis, sizeof dis, "%s\\disabled", s_tex);
    CreateDirectoryA(dis, NULL);
    snprintf(from, sizeof from, "%s\\%s", p->on ? s_tex : dis, p->name);
    snprintf(to, sizeof to, "%s\\%s", p->on ? dis : s_tex, p->name);
    if (MoveFileExA(from, to, 0)) p->on = !p->on;
    else msg("Could not move %s (is the game running?)", p->name);
#else
    char from[1200], to[1200], dis[1200];
    Pack *p = &s_packs[i];
    snprintf(dis, sizeof dis, "%s/disabled", s_tex);
    mkdir(dis, 0755);
    snprintf(from, sizeof from, "%s/%s", p->on ? s_tex : dis, p->name);
    snprintf(to, sizeof to, "%s/%s", p->on ? dis : s_tex, p->name);
    if (!rename(from, to)) p->on = !p->on;
    else msg("Could not move %s (is the game running?)", p->name);
#endif
}

/* ---------------------------------------------------------------- update check */
static SDL_atomic_t s_upd_state;   /* 0 checking, 1 up to date, 2 newer, 3 offline, 4 no install folder, 5 Linux (no updater) */
static char s_upd_tag[64], s_have[64];

#ifdef _WIN32
static int run_capture(const char *cmdline, const char *cwd, char *out, int outn)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD n, code = 1;
    int got = 0;
    char cl[2048];
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    snprintf(cl, sizeof cl, "%s", cmdline);
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    while (got < outn - 1 && ReadFile(rd, out + got, (DWORD)(outn - 1 - got), &n, NULL) && n) got += (int)n;
    out[got] = 0;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return (int)code;
}
#endif

static int SDLCALL update_check(void *u)
{
    (void)u;
#ifdef _WIN32
    char cmd[2600], out[1024], *p;
    if (!s_repo[0]) { SDL_AtomicSet(&s_upd_state, 4); return 0; }
    snprintf(cmd, sizeof cmd, "%s\\installer\\python\\tools\\python.exe", s_repo);
    if (!file_exists(cmd)) { SDL_AtomicSet(&s_upd_state, 4); return 0; }
    snprintf(cmd, sizeof cmd, "\"%s\\installer\\python\\tools\\python.exe\" -I \"%s\\installer\\update.py\" --check",
             s_repo, s_repo);
    run_capture(cmd, s_repo, out, sizeof out);
    if ((p = strstr(out, "HAVE "))) sscanf(p + 5, "%63s", s_have);
    if ((p = strstr(out, "LATEST "))) {
        int newer = 0;
        sscanf(p + 7, "%63s", s_upd_tag);
        if ((p = strstr(out, "NEWER "))) newer = atoi(p + 6);
        SDL_AtomicSet(&s_upd_state, newer ? 2 : 1);
    } else SDL_AtomicSet(&s_upd_state, 3);
#else
    SDL_AtomicSet(&s_upd_state, 5);   /* Linux test builds: no updater yet */
#endif
    return 0;
}

/* ---------------------------------------------------------------- packs to download */
/* installer/packs.py (the installer's private Python) lists packs.json and downloads one pack
 * at a time; the launcher reads its PROGRESS / DONE / FAIL lines. */
typedef struct { char id[32], name[96], about[160]; long long size; int installed; } Avail;
static Avail s_av[32];
static int s_nav;
static SDL_atomic_t s_av_ready;        /* 0 not asked, 1 listing, 2 listed */
static SDL_atomic_t s_dl_pct[32];      /* per pack: -1 idle, 0..100 downloading */
static SDL_atomic_t s_dl_state[32];    /* 0 idle, 1 queued, 2 downloading, 3 done, 4 failed */
static SDL_atomic_t s_rescan;          /* a download finished: rescan the textures folder */
static SDL_atomic_t s_dl_busy;

static void py_cmd(char *out, size_t n, const char *args)
{
    snprintf(out, n, "\"%s\\installer\\python\\tools\\python.exe\" -I \"%s\\installer\\packs.py\" %s", s_repo, s_repo, args);
}

static int SDLCALL avail_list(void *u)
{
    (void)u;
#ifdef _WIN32
    char cmd[2600], args[1300], out[8192], *line;
    int n = 0;
    snprintf(args, sizeof args, "--list \"%s\"", s_tex);
    py_cmd(cmd, sizeof cmd, args);
    if (s_repo[0] && run_capture(cmd, s_repo, out, sizeof out) >= 0)
        for (line = strtok(out, "\r\n"); line && n < 32; line = strtok(NULL, "\r\n")) {
            Avail *a = &s_av[n];
            char rest[300] = "", *bar;
            if (sscanf(line, "PACK %31s %d %lld %299[^\n]", a->id, &a->installed, &a->size, rest) != 4) continue;
            if ((bar = strchr(rest, '|'))) { *bar = 0; snprintf(a->about, sizeof a->about, "%s", bar + 1); }
            snprintf(a->name, sizeof a->name, "%s", rest);
            SDL_AtomicSet(&s_dl_pct[n], -1);
            n++;
        }
    s_nav = n;
#endif
    SDL_AtomicSet(&s_av_ready, 2);
    return 0;
}

/* runs every queued download in turn, reading packs.py's progress lines as they come */
static int SDLCALL dl_worker(void *u)
{
    (void)u;
#ifdef _WIN32
    int i, any = 1;
    while (any) {
        any = 0;
        for (i = 0; i < s_nav; i++) {
            SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
            HANDLE rd, wr;
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            char cmd[2600], args[1300], buf[512], line[512];
            DWORD n;
            int ll = 0, ok = 0;
            if (SDL_AtomicGet(&s_dl_state[i]) != 1) continue;
            any = 1;
            SDL_AtomicSet(&s_dl_state[i], 2);
            SDL_AtomicSet(&s_dl_pct[i], 0);
            snprintf(args, sizeof args, "--get %s \"%s\"", s_av[i].id, s_tex);
            py_cmd(cmd, sizeof cmd, args);
            if (!CreatePipe(&rd, &wr, &sa, 0)) { SDL_AtomicSet(&s_dl_state[i], 4); continue; }
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            memset(&si, 0, sizeof si);
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = wr;
            if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, s_repo, &si, &pi)) {
                CloseHandle(rd); CloseHandle(wr);
                SDL_AtomicSet(&s_dl_state[i], 4);
                continue;
            }
            CloseHandle(wr);
            while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
                DWORD k;
                for (k = 0; k < n; k++) {
                    if (buf[k] == '\n' || ll == (int)sizeof line - 1) {
                        long long got, tot;
                        line[ll] = 0;
                        if (sscanf(line, "PROGRESS %*s %lld %lld", &got, &tot) == 2 && tot > 0)
                            SDL_AtomicSet(&s_dl_pct[i], (int)(got * 100 / tot));
                        else if (!strncmp(line, "DONE", 4)) ok = 1;
                        ll = 0;
                    } else if (buf[k] != '\r') line[ll++] = buf[k];
                }
            }
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
            SDL_AtomicSet(&s_dl_state[i], ok ? 3 : 4);
            if (ok) s_av[i].installed = 1;
            SDL_AtomicSet(&s_rescan, 1);
        }
    }
#endif
    SDL_AtomicSet(&s_dl_busy, 0);
    return 0;
}

static void dl_queue(int i)
{
    int st = SDL_AtomicGet(&s_dl_state[i]);
    if (s_av[i].installed || st == 1 || st == 2) return;
    SDL_AtomicSet(&s_dl_state[i], 1);
    if (SDL_AtomicCAS(&s_dl_busy, 0, 1)) SDL_DetachThread(SDL_CreateThread(dl_worker, "download", NULL));
}

/* ---------------------------------------------------------------- drag and drop
 * A file or folder dropped on the window goes to installer/mods.py: a pack for this port is
 * copied into textures, a PS2 texture mod (PCSX2 replacements, like the remaster) is converted
 * into packs. Its STEP / PROGRESS / DONE / FAIL lines drive the box drawn over every page. */
#define MAXDROP 16
static char *s_drop[MAXDROP];
static int s_ndrop;
static SDL_atomic_t s_conv;            /* 0 idle, 1 working, 2 done, 3 failed */
static SDL_atomic_t s_conv_pct;        /* -1 no figure */
static char s_conv_line[256];          /* the last STEP, then the DONE / FAIL text */
static SDL_mutex *s_conv_mx;

static void conv_set(const char *t)
{
    SDL_LockMutex(s_conv_mx);
    snprintf(s_conv_line, sizeof s_conv_line, "%s", t);
    SDL_UnlockMutex(s_conv_mx);
}

static void conv_line(const char *line)
{
    long long a, b;
    if (!strncmp(line, "STEP ", 5)) { conv_set(line + 5); SDL_AtomicSet(&s_conv_pct, -1); }
    else if (sscanf(line, "PROGRESS %lld %lld", &a, &b) == 2 && b > 0) SDL_AtomicSet(&s_conv_pct, (int)(a * 100 / b));
    else if (!strncmp(line, "DONE ", 5)) { conv_set(line + 5); SDL_AtomicSet(&s_conv, 2); }
    else if (!strncmp(line, "FAIL ", 5)) { conv_set(line + 5); SDL_AtomicSet(&s_conv, 3); }
}

static int SDLCALL conv_worker(void *u)
{
    char **paths = u, cmd[8192], line[512];
    int i, ll = 0;
    size_t n;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[512];
    DWORD got;
    n = (size_t)snprintf(cmd, sizeof cmd, "\"%s\\installer\\python\\tools\\python.exe\" -I \"%s\\installer\\mods.py\" \"%s\"",
                         s_repo, s_repo, s_dir);
#else
    FILE *p;
    char buf[512];
    size_t got;
    n = (size_t)snprintf(cmd, sizeof cmd, "python3 -I '%s/installer/mods.py' '%s'", s_repo, s_dir);
#endif
    for (i = 0; paths[i] && n < sizeof cmd - 1; i++)
#ifdef _WIN32
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, " \"%s\"", paths[i]);
#else
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, " '%s'", paths[i]);
#endif
    for (i = 0; paths[i]; i++) SDL_free(paths[i]);
    free(paths);
    if (n >= sizeof cmd - 1) { conv_set("Too many files at once: drop fewer."); SDL_AtomicSet(&s_conv, 3); return 0; }
#ifdef _WIN32
    if (!CreatePipe(&rd, &wr, &sa, 0)) { conv_set("Couldn't start the converter."); SDL_AtomicSet(&s_conv, 3); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, s_repo, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        conv_set("Couldn't start the converter (run Install.bat once).");
        SDL_AtomicSet(&s_conv, 3);
        return 0;
    }
    CloseHandle(wr);
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
#else
    if (!(p = popen(cmd, "r"))) { conv_set("Couldn't start the converter."); SDL_AtomicSet(&s_conv, 3); return 0; }
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) {
#endif
        size_t k;
        for (k = 0; k < (size_t)got; k++) {
            if (buf[k] == '\n' || ll == (int)sizeof line - 1) { line[ll] = 0; conv_line(line); ll = 0; }
            else if (buf[k] != '\r') line[ll++] = buf[k];
        }
    }
#ifdef _WIN32
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
#else
    pclose(p);
#endif
    if (SDL_AtomicGet(&s_conv) == 1) { conv_set("The converter stopped without finishing."); SDL_AtomicSet(&s_conv, 3); }
    SDL_AtomicSet(&s_rescan, 1);
    return 0;
}

/* SDL_DROPCOMPLETE: hand what was dropped to the converter */
static void drop_go(void)
{
    char **paths;
    int i;
    if (!s_ndrop) return;
    if (SDL_AtomicGet(&s_conv) == 1 || !s_repo[0]) {
        msg("%s", SDL_AtomicGet(&s_conv) == 1 ? "Still working on the last drop." : "Run Install.bat once to add mods.");
        for (i = 0; i < s_ndrop; i++) SDL_free(s_drop[i]);
        s_ndrop = 0;
        return;
    }
    paths = calloc((size_t)s_ndrop + 1, sizeof *paths);
    for (i = 0; i < s_ndrop; i++) paths[i] = s_drop[i];
    s_ndrop = 0;
    if (!s_conv_mx) s_conv_mx = SDL_CreateMutex();
    conv_set("Starting");
    SDL_AtomicSet(&s_conv_pct, -1);
    SDL_AtomicSet(&s_conv, 1);
    SDL_DetachThread(SDL_CreateThread(conv_worker, "mods", paths));
}

/* ---------------------------------------------------------------- actions */
static int s_quit;
#ifndef _WIN32
static char s_play_path[1200];   /* Linux: the game to exec once the launcher window is closed */
#endif

static void act_play(void)
{
#ifdef _WIN32
    char exe[1200];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    if (s_ini_dirty) ini_save();
    snprintf(exe, sizeof exe, "%s\\pcj.exe", s_dir);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, s_dir, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        s_quit = 1;
    } else msg("Could not start pcj.exe%s", " (is it next to the launcher?)");
#else
    /* the game replaces the launcher in the same process after the window closes (main): Steam,
     * which started the launcher, then keeps tracking the game */
    char p[1200];
    if (s_ini_dirty) ini_save();
    snprintf(p, sizeof p, "%s/PCJ-Linux.sh", s_dir);
    if (!file_exists(p)) snprintf(p, sizeof p, "%s/pcj-linux", s_dir);
    if (!file_exists(p)) snprintf(p, sizeof p, "%s/pcj", s_dir);
    if (!file_exists(p)) { msg("Could not find the game%s", " (PCJ-Linux.sh or pcj-linux next to the launcher)"); return; }
    snprintf(s_play_path, sizeof s_play_path, "%s", p);
    s_quit = 1;
#endif
}

static void act_update(void)
{
#ifdef _WIN32
    char bat[1200], relaunch[1200];
    if (!s_repo[0]) { msg("The install folder is missing%s", ": download the latest release and run Install.bat"); return; }
    snprintf(bat, sizeof bat, "%s\\Update.bat", s_repo);
    snprintf(relaunch, sizeof relaunch, "%s\\launcher.exe", s_dir);
    SetEnvironmentVariableA("PCJ_RELAUNCH", relaunch);   /* the installer reopens the launcher when done */
    if ((INT_PTR)ShellExecuteA(NULL, "open", bat, NULL, s_repo, SW_SHOWNORMAL) > 32) s_quit = 1;
    else msg("Could not start %s", bat);
#else
    msg("Linux: put the new build's files into this folder%s", " (your save, settings and packs stay)");
#endif
}

/* stored (uncompressed) zip writer for the problem report */
typedef struct { char name[260]; uint32_t crc, size, off; } ZEnt;
static uint32_t crc32_buf(uint32_t c, const unsigned char *p, size_t n)
{
    static uint32_t t[256];
    size_t i;
    if (!t[1]) for (i = 0; i < 256; i++) { uint32_t r = (uint32_t)i; int k; for (k = 0; k < 8; k++) r = r & 1 ? 0xEDB88320u ^ (r >> 1) : r >> 1; t[i] = r; }
    c = ~c;
    for (i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static int zip_add(FILE *z, ZEnt *e, const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (64L << 20) || !(b = malloc((size_t)n + 1))) { fclose(f); return 0; }
    n = (long)fread(b, 1, (size_t)n, f);
    fclose(f);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = crc32_buf(0, b, (size_t)n);
    e->size = (uint32_t)n;
    e->off = (uint32_t)ftell(z);
    put32(z, 0x04034b50); put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0x21);
    put32(z, e->crc); put32(z, e->size); put32(z, e->size); put16(z, (unsigned)strlen(name)); put16(z, 0);
    fwrite(name, 1, strlen(name), z);
    fwrite(b, 1, (size_t)n, z);
    free(b);
    return 1;
}

static void act_report(void)
{
#ifdef _WIN32
    char logs[1200], pat[1300], best[260] = "", sess[1300], zp[1200], desk[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILE *z;
    ZEnt ents[32];
    int ne = 0, i;
    uint32_t cd;
    SYSTEMTIME st;
    snprintf(logs, sizeof logs, "%s\\logs", s_dir);
    snprintf(pat, sizeof pat, "%s\\session-*", logs);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, best) > 0)
            snprintf(best, sizeof best, "%s", fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!best[0]) { msg("No game logs yet%s", ": play once, then report"); return; }
    /* the report zip goes in the install's own logs folder, next to the sessions it packs (one file
     * because a GitHub issue takes attachments, not folders) */
    snprintf(desk, sizeof desk, "%s", logs);
    GetLocalTime(&st);
    snprintf(zp, sizeof zp, "%s\\pcj-report-%04d%02d%02d-%02d%02d.zip", desk, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    if (!(z = fopen(zp, "wb"))) { msg("Could not write %s", zp); return; }
    snprintf(sess, sizeof sess, "%s\\%s", logs, best);
    {
        static const char *files[] = {"session.txt", "summary.txt", "events.jsonl", "frames.csv", "pcj-log.txt", "prof.txt"};
        char p[1400], nm[400];
        for (i = 0; i < 6; i++) {
            snprintf(p, sizeof p, "%s\\%s", sess, files[i]);
            snprintf(nm, sizeof nm, "%s/%s", best, files[i]);
            if (ne < 32 && zip_add(z, &ents[ne], p, nm)) ne++;
        }
        snprintf(p, sizeof p, "%s\\pcj.ini", s_dir);   /* settings: no paths in it unless the player put one there */
        if (ne < 32 && zip_add(z, &ents[ne], p, "pcj.ini")) ne++;
        snprintf(p, sizeof p, "%s\\apiverify.txt", s_dir);   /* native renderer verifier (CJ_API_VERIFY=1), if it ran */
        if (ne < 32 && zip_add(z, &ents[ne], p, "apiverify.txt")) ne++;
    }
    cd = (uint32_t)ftell(z);
    for (i = 0; i < ne; i++) {
        put32(z, 0x02014b50); put16(z, 20); put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0x21);
        put32(z, ents[i].crc); put32(z, ents[i].size); put32(z, ents[i].size); put16(z, (unsigned)strlen(ents[i].name));
        put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0); put32(z, 0); put32(z, ents[i].off);
        fwrite(ents[i].name, 1, strlen(ents[i].name), z);
    }
    {
        uint32_t cdlen = (uint32_t)ftell(z) - cd;
        put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, (unsigned)ne); put16(z, (unsigned)ne);
        put32(z, cdlen); put32(z, cd); put16(z, 0);
    }
    fclose(z);
    ShellExecuteA(NULL, "open", ISSUES_URL, NULL, NULL, SW_SHOWNORMAL);
    {
        char sel[1300];
        snprintf(sel, sizeof sel, "/select,\"%s\"", zp);
        ShellExecuteA(NULL, "open", "explorer.exe", sel, NULL, SW_SHOWNORMAL);
    }
    msg("Saved %s: attach it to the issue that just opened", strrchr(zp, '\\') + 1);
#else
    char logs[1200], best[260] = "", sess[1300], zp[1300], p[1400], nm[400];
    static const char *files[] = {"session.txt", "summary.txt", "events.jsonl", "frames.csv", "pcj-log.txt", "prof.txt"};
    DIR *dp;
    struct dirent *de;
    FILE *z;
    ZEnt ents[32];
    int ne = 0, i;
    uint32_t cd;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    snprintf(logs, sizeof logs, "%s/logs", s_dir);
    if ((dp = opendir(logs))) {
        while ((de = readdir(dp)))
            if (!strncmp(de->d_name, "session-", 8) && strcmp(de->d_name, best) > 0) snprintf(best, sizeof best, "%s", de->d_name);
        closedir(dp);
    }
    if (!best[0]) { msg("No game logs yet%s", ": play once, then report"); return; }
    snprintf(zp, sizeof zp, "%s/pcj-report-%04d%02d%02d-%02d%02d.zip", logs, lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min);
    if (!(z = fopen(zp, "wb"))) { msg("Could not write %s", zp); return; }
    snprintf(sess, sizeof sess, "%s/%s", logs, best);
    for (i = 0; i < 6; i++) {
        snprintf(p, sizeof p, "%s/%s", sess, files[i]);
        snprintf(nm, sizeof nm, "%s/%s", best, files[i]);
        if (ne < 32 && zip_add(z, &ents[ne], p, nm)) ne++;
    }
    snprintf(p, sizeof p, "%s/pcj.ini", s_dir);
    if (ne < 32 && zip_add(z, &ents[ne], p, "pcj.ini")) ne++;
    snprintf(p, sizeof p, "%s/pcj-linux-log.txt", s_dir);
    if (ne < 32 && zip_add(z, &ents[ne], p, "pcj-linux-log.txt")) ne++;
    cd = (uint32_t)ftell(z);
    for (i = 0; i < ne; i++) {
        put32(z, 0x02014b50); put16(z, 20); put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0x21);
        put32(z, ents[i].crc); put32(z, ents[i].size); put32(z, ents[i].size); put16(z, (unsigned)strlen(ents[i].name));
        put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0); put32(z, 0); put32(z, ents[i].off);
        fwrite(ents[i].name, 1, strlen(ents[i].name), z);
    }
    {
        uint32_t cdlen = (uint32_t)ftell(z) - cd;
        put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, (unsigned)ne); put16(z, (unsigned)ne);
        put32(z, cdlen); put32(z, cd); put16(z, 0);
    }
    fclose(z);
    open_with_system(ISSUES_URL);
    open_with_system(logs);
    msg("Saved %s: attach it to the issue that just opened", strrchr(zp, '/') + 1);
#endif
}


/* ---------------------------------------------------------------- drawing */
static SDL_Renderer *R;
typedef struct { stbtt_bakedchar ch[224]; SDL_Texture *tex; int px; } Font;   /* 32..255: Latin-1 */
static Font F_title, F_big, F_ui, F_small;

static int font_bake(Font *f, const unsigned char *ttf, float px)
{
    int tw = 1024, th = 1024, i;
    unsigned char *a = malloc((size_t)tw * th);
    Uint32 *rgba = malloc((size_t)tw * th * 4);
    f->px = (int)px;
    memset(a, 0, (size_t)tw * th);
    stbtt_BakeFontBitmap(ttf, stbtt_GetFontOffsetForIndex(ttf, 0), px, a, tw, th, 32, 224, f->ch);
    for (i = 0; i < tw * th; i++) rgba[i] = 0x00FFFFFFu | ((Uint32)a[i] << 24);
    f->tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
    SDL_UpdateTexture(f->tex, NULL, rgba, tw * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(f->tex, SDL_ScaleModeLinear);
    free(a); free(rgba);
    return 1;
}

static float text_w(Font *f, const char *s)
{
    float x = 0, y = 0;
    stbtt_aligned_quad q;
    const unsigned char *u = (const unsigned char *)tr(s);
    for (; *u; u++) if (*u >= 32 && (*u < 128 || *u >= 160)) stbtt_GetBakedQuad(f->ch, 1024, 1024, *u - 32, &x, &y, &q, 1);
    return x;
}

/* align: 0 left, 1 centre, 2 right; y = baseline */
static void text(Font *f, float x, float y, Uint32 col, int align, const char *s)
{
    stbtt_aligned_quad q;
    const unsigned char *u;
    if (align) x -= text_w(f, s) * (align == 1 ? 0.5f : 1.0f);
    s = tr(s);
    u = (const unsigned char *)s;
    SDL_SetTextureColorMod(f->tex, col >> 16 & 255, col >> 8 & 255, col & 255);
    SDL_SetTextureAlphaMod(f->tex, col >> 24 & 255);
    for (; *u; u++) {
        SDL_Rect src;
        SDL_FRect dst;
        if (*u < 32 || (*u >= 128 && *u < 160)) continue;
        stbtt_GetBakedQuad(f->ch, 1024, 1024, *u - 32, &x, &y, &q, 1);
        src.x = (int)(q.s0 * 1024 + 0.5f); src.y = (int)(q.t0 * 1024 + 0.5f);
        src.w = (int)((q.s1 - q.s0) * 1024 + 0.5f); src.h = (int)((q.t1 - q.t0) * 1024 + 0.5f);
        dst.x = q.x0; dst.y = q.y0; dst.w = q.x1 - q.x0; dst.h = q.y1 - q.y0;
        SDL_RenderCopyF(R, f->tex, &src, &dst);
    }
}

static void fill(float x, float y, float w, float h, Uint32 c)
{
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawColor(R, c >> 16 & 255, c >> 8 & 255, c & 255, c >> 24 & 255);
    SDL_RenderFillRectF(R, &r);
}

static void frame(float x, float y, float w, float h, Uint32 c)
{
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawColor(R, c >> 16 & 255, c >> 8 & 255, c & 255, c >> 24 & 255);
    SDL_RenderDrawRectF(R, &r);
}

#define RED 0xFFD4141Eu
#define RED_DIM 0xFF6E0C10u
#define TXT 0xFFE8E2DCu
#define TXT_DIM 0xFF8C8580u
#define PANEL 0xC0100A0Au


/* ---------------------------------------------------------------- art from the player's disc */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"

static SDL_Texture *s_bg, *s_logo;
static int s_logo_w, s_logo_h;

static SDL_Texture *load_png(const char *name, int *w, int *h)
{
    char p[1200];
    int n;
    unsigned char *px;
    SDL_Texture *t;
    snprintf(p, sizeof p, "%s" SEP "launcher-art" SEP "%s", s_dir, name);
    if (!(px = stbi_load(p, w, h, &n, 4))) return NULL;
    t = SDL_CreateTexture(R, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, *w, *h);
    SDL_UpdateTexture(t, NULL, px, *w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    stbi_image_free(px);
    return t;
}

/* The game's loading screen: the hologram Predator across the window, the logo crisp on top,
 * a dark fade on the left for the menu, scanlines and a slow scan band. */
static void background(Uint32 t, int small_logo)
{
    int y;
    fill(0, 0, W, H, 0xFF000000u);
    if (s_bg) {
        SDL_Rect src = {0, 24, 512, 288};   /* 960x540 window = 512x288 of the 512x512 art */
        SDL_RenderCopy(R, s_bg, &src, NULL);
    }
    /* left fade: hides the art's own (soft) logo and gives the menu a dark field */
    {
        int x;
        for (x = 0; x < 620; x += 6) {
            float k = 1.0f - (float)x / 620;
            Uint32 a = (Uint32)(250 * powf(k, 1.25f));
            fill((float)x, 0, 6, H, a << 24);
        }
    }
    {   /* the art carries its own soft logo top left: black it out under ours, soft edges */
        int i;
        fill(0, 0, 560, 236, 0xFF000000u);
        for (i = 0; i < 120; i += 4) fill(560.0f + i, 0, 4, 236, (Uint32)(255 * (1.0f - i / 120.0f)) << 24);
        for (i = 0; i < 36; i += 3) {   /* bottom edge, with the right edge's fade continued into the corner */
            float fv = 1.0f - i / 36.0f;
            int j;
            fill(0, 236.0f + i, 560, 3, (Uint32)(255 * fv) << 24);
            for (j = 0; j < 120; j += 4) fill(560.0f + j, 236.0f + i, 4, 3, (Uint32)(255 * fv * (1.0f - j / 120.0f)) << 24);
        }
    }
    /* scanlines + a band moving down, like the game's hologram screens */
    for (y = 0; y < H; y += 3) fill(0, (float)y, W, 1, 0x22000000u);
    {
        float band = (float)((t / 12) % (H + 120)) - 60;
        fill(0, band, W, 40, 0x0A60A0FFu);
        fill(0, band + 14, W, 6, 0x0C80C0FFu);
    }
    if (s_logo) {
        float lw = small_logo ? 210 : 400, lh = lw * s_logo_h / s_logo_w;
        SDL_FRect d = {small_logo ? 44.0f : 52.0f, small_logo ? 14.0f : 30.0f, lw, lh};
        SDL_RenderCopyF(R, s_logo, NULL, &d);
    } else {
        text(&F_title, 56, 110, RED, 0, "PREDATOR");
        text(&F_ui, 60, 140, TXT, 0, "C O N C R E T E   J U N G L E");
    }
}

/* ---------------------------------------------------------------- menus */
/* Every page is a list of rows, laid out and driven the same way: mouse hover or up/down
 * picks a row, Enter / A / click activates, left/right change a value, Esc / B goes back. */
typedef struct { const char *label; int kind; int idx; } Row;   /* kind: 0 action, 1 setting, 2 pack */
enum { HOME, SETTINGS, PACKS, TESTS, EXPER };
enum { A_PLAY, A_UPDATE, A_SETTINGS, A_PACKS, A_REPORT, A_QUIT, A_BACK, A_OPENTEX, A_CHECK, A_DECK, A_TESTS, A_EXPER };
static int s_page, s_sel, s_top;
static int mx, my, mclick, mmoved, s_hot;
static int k_up, k_down, k_left, k_right, k_ok, k_back, k_lang;   /* k_lang: Y / L switches the language */

static void red_dots(float x, float y)
{
    /* the game's cursor: three red dots before the selected row */
    fill(x, y - 3, 5, 5, RED);
    fill(x + 9, y - 3, 5, 5, 0xFFB01018u);
    fill(x + 18, y - 3, 5, 5, 0xFF7A0C10u);
}

static void footer(const char *ok, const char *back)
{
    float x = 52, y = H - 26;
    fill(0, H - 44, W, 44, 0xB0000000u);
    fill(0, H - 44, W, 1, 0x60D4141Eu);
    /* drawn button glyphs (A green, B red), the port's own */
    fill(x, y - 10, 20, 20, 0xFF2E9E3Eu);
    text(&F_small, x + 10, y + 5, 0xFFFFFFFFu, 1, "A");
    text(&F_small, x + 28, y + 5, TXT, 0, ok);
    if (back) {
        float bx = x + 40 + text_w(&F_small, ok) + 30;
        fill(bx, y - 10, 20, 20, 0xFFB8202Au);
        text(&F_small, bx + 10, y + 5, 0xFFFFFFFFu, 1, "B");
        text(&F_small, bx + 28, y + 5, TXT, 0, back);
    }
}

static void status_box(void)
{
    char line[160];
    int st = SDL_AtomicGet(&s_upd_state);
    float x = W - 300, y = H - 21;
    snprintf(line, sizeof line, "v%s", s_have[0] ? s_have : "?");
    text(&F_small, W - 40, y, TXT_DIM, 2, line);
    if (st == 0) snprintf(line, sizeof line, "Checking for updates");
    else if (st == 1) snprintf(line, sizeof line, "Up to date");
    else if (st == 2) snprintf(line, sizeof line, tr("Update available: %s"), s_upd_tag);
    else if (st == 3) snprintf(line, sizeof line, "Could not reach GitHub");
    else if (st == 5) snprintf(line, sizeof line, "Linux test build: update by copying a new build in");
    else snprintf(line, sizeof line, "Run Install.bat once to enable updates");
    text(&F_small, W - 40 - text_w(&F_small, "v0.0.00") - 24, y, st == 2 ? 0xFFFF6A60u : TXT_DIM, 2, line);
    (void)x;
}

/* draw rows at x/y with spacing dy; returns the activated row or -1. Selection follows the
 * mouse when it moves, the keys otherwise. */
static int rows(const Row *r, int n, float x, float y, float dy, Font *f, float vx, int visible)
{
    int i, act = -1;
    if (n == 0) return -1;
    if (k_up) do s_sel = (s_sel + n - 1) % n; while (r[s_sel].kind == 4);
    if (k_down) do s_sel = (s_sel + 1) % n; while (r[s_sel].kind == 4);
    if (r[s_sel % n].kind == 4) s_sel = (s_sel + 1) % n;   /* never rest on a heading */
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + visible) s_top = s_sel - visible + 1;
    for (i = s_top; i < n && i < s_top + visible; i++) {
        float ry = y + (i - s_top) * dy;
        int over = mx >= x - 40 && mx < (vx > 0 ? vx + 160 : x + 420) && my >= ry - dy * 0.7f && my < ry + dy * 0.3f;
        if (r[i].kind == 4) {   /* a heading: not selectable */
            text(&F_small, x, ry - 2, TXT_DIM, 0, r[i].label);
            continue;
        }
        if (over && mmoved) s_sel = i;
        if (over) s_hot = 1;
        if (over && mclick) act = i;
        if (i == s_sel) red_dots(x - 34, ry - f->px * 0.32f);
        text(f, x, ry, i == s_sel ? RED : TXT, 0, r[i].label);
    }
    if (k_ok) act = s_sel;
    return act;
}

static const char *val_str(int i)
{
    static char b[32];
    if (!strcmp(S[i].key, "CJ_LANG")) { static char pt[32]; l1("Português (Brasil)", pt, sizeof pt); return s_val[i] ? pt : "English"; }
    if (S[i].lo == 0 && S[i].hi == 1) return s_val[i] ? "On" : "Off";
    if (!strcmp(S[i].key, "CJ_GL_SCALE")) {
        static const char *r[] = {"", "1x  (640x480)", "2x  (1280x960)", "3x  (1440p)", "4x  (2560x1920)", "5x  (4K)"};
        return r[s_val[i]];
    }
    snprintf(b, sizeof b, "%d", s_val[i]);
    return b;
}

static void change(int i, int dir)
{
    int v = s_val[i] + dir * S[i].step;
    if (S[i].lo == 0 && S[i].hi == 1) v = !s_val[i];
    if (v < S[i].lo || v > S[i].hi) return;
    s_val[i] = v;
    s_ini_dirty = 1;
}

/* ---------------------------------------------------------------- Steam Deck */
/* Steam sets SteamDeck=1 for games on a Deck; outside Steam the board ID says so (Proton maps the
 * Linux root to Z:, so /sys is readable from here): vendor Valve, product Jupiter (LCD) or Galileo (OLED). */
static int read_first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}
static int is_steam_deck(void)
{
    static int cached = -1;
    const char *e = getenv("SteamDeck");
    char v[64], p[64];
    if (cached >= 0) return cached;
    cached = 1;
    if (e && *e == '1') return 1;
#ifdef _WIN32
#define DMI "Z:\\sys\\devices\\virtual\\dmi\\id\\"
#else
#define DMI "/sys/devices/virtual/dmi/id/"
#endif
    if (read_first_line(DMI "board_vendor", v, sizeof v) && strstr(v, "Valve")
        && read_first_line(DMI "product_name", p, sizeof p)
        && (strstr(p, "Jupiter") || strstr(p, "Galileo"))) return 1;
    return cached = 0;
}
/* The Deck preset: the CPU (four Zen 2 cores running the game's own code) is the limit, so nothing
 * here costs CPU; 2x is 1280x960, closest to the Deck's 800 lines. */
static void deck_preset(void)
{
    static const struct { const char *key; int v; } P[] = {
        {"CJ_FPS", 60}, {"CJ_GL_SCALE", 2}, {"CJ_WIDESCREEN", 1}, {"CJ_FULLSCREEN", 1}, {"CJ_VSYNC", 1},
        {"CJ_TEX_REPLACE", 1}, {"CJ_MOUSE", 0}, {"CJ_GLYPHS", 1}, {"CJ_CAM_MODERN", 1}, {"CJ_API_TAKEOVER", 0}, {"CJ_GL_SHADER_CACHE", 1},
    };
    int i, k;
    ini_load();
    for (k = 0; k < (int)(sizeof P / sizeof P[0]); k++)
        for (i = 0; i < NS; i++) if (!strcmp(S[i].key, P[k].key)) s_val[i] = P[k].v;
    s_ini_dirty = 1;
    ini_save();
}
/* first run on a Deck: the preset once (a marker file remembers it), never over the player's own choices later */
static void deck_first_run(void)
{
    char mk[1300];
    FILE *f;
    if (!is_steam_deck()) return;
    snprintf(mk, sizeof mk, "%s" SEP "deck-preset-applied.txt", s_dir);
    if (file_exists(mk)) return;
    deck_preset();
    if ((f = fopen(mk, "w"))) { fputs("The Steam Deck preset was applied on the first run. Delete this file to apply it again.\n", f); fclose(f); }
    msg("Steam Deck detected: Deck settings applied.%s", " Change them in PC Settings.");
}

static void go(int page)
{
    if ((s_page == SETTINGS || s_page == EXPER) && s_ini_dirty) ini_save();
    s_page = page;
    s_sel = s_top = 0;
    if (page == SETTINGS) ini_load();
    if (page == PACKS) packs_scan();
}

static int s_check_asked;   /* the player pressed Check for Updates: say how it went */

/* EN | PT in the home page's top right: click it, or press Y / L. Same setting as PC Settings >
 * Language (CJ_LANG): the launcher and the game's text follow it. */
static void lang_toggle(void)
{
    int k, li = -1;
    float x = W - 132, y = 22, w = 50, h = 26;
    for (k = 0; k < NS; k++) if (!strcmp(S[k].key, "CJ_LANG")) li = k;
    if (li < 0) return;
    {
        int over = mx >= x && mx < x + 2 * w && my >= y && my < y + h;
        if (over) s_hot = 1;
        if (k_lang || (over && mclick)) {
            s_val[li] = !s_val[li];
            s_ini_dirty = 1;
            ini_save();
            mclick = 0;
        }
    }
    for (k = 0; k < 2; k++) {
        int on = s_val[li] == k;
        fill(x + k * w, y, w, h, on ? RED : 0xA0101010u);
        frame(x + k * w, y, w, h, on ? RED : 0x60FFFFFFu);
        text(&F_small, x + k * w + w / 2, y + 18, on ? 0xFFFFFFFFu : TXT_DIM, 1, k ? "PT" : "EN");
    }
}

static void page_home(void)
{
    Row r[10];
    int n = 0, a;
    int upd = SDL_AtomicGet(&s_upd_state) == 2;
    int tests = is_steam_deck() || (getenv("CJ_PERF_TESTS") && *getenv("CJ_PERF_TESTS") == '1');
    static char ulabel[80];
    r[n++] = (Row){"Play", 0, A_PLAY};
    if (upd) { snprintf(ulabel, sizeof ulabel, tr("Update to %s"), s_upd_tag); r[n++] = (Row){ulabel, 0, A_UPDATE}; }
    r[n++] = (Row){"PC Settings", 0, A_SETTINGS};
    r[n++] = (Row){"Texture Packs", 0, A_PACKS};
    r[n++] = (Row){"Check for Updates", 0, A_CHECK};
    if (tests) r[n++] = (Row){"Performance Tests", 0, A_TESTS};
    r[n++] = (Row){"Report a Problem", 0, A_REPORT};
    r[n++] = (Row){"Quit", 0, A_QUIT};
    a = rows(r, n, 92, 282, n > 6 ? 30 : 35, &F_big, 0, 9);
    if (a >= 0) switch (r[a].idx) {
        case A_PLAY: act_play(); break;
        case A_UPDATE: act_update(); break;
        case A_SETTINGS: go(SETTINGS); break;
        case A_PACKS: go(PACKS); break;
        case A_TESTS: go(TESTS); break;
        case A_REPORT: act_report(); break;
        case A_CHECK:
            if (upd) act_update();
            else if (SDL_AtomicGet(&s_upd_state) != 0) {   /* ask GitHub again */
                s_check_asked = 1;
                SDL_AtomicSet(&s_upd_state, 0);
                SDL_DetachThread(SDL_CreateThread(update_check, "update", NULL));
            }
            break;
        case A_QUIT: s_quit = 1; break;
        }
    if (s_check_asked && SDL_AtomicGet(&s_upd_state) != 0) {
        int st = SDL_AtomicGet(&s_upd_state);
        s_check_asked = 0;
        if (st == 1) msg("You have the latest version.%s", "");
        else if (st == 2) msg("A new version is ready: choose Update to %s", s_upd_tag);
        else if (st == 3) msg("Could not reach GitHub.%s", " Check the internet connection.");
        else if (st == 5) msg("Linux test build: copy the new build's files into this folder%s", "");
        else msg("Updates need the install folder.%s", " Run Install.bat once.");
    }
    if (k_back) s_quit = 1;
    footer("Select", "Quit");
    status_box();
    lang_toggle();
}

/* PC Settings (exper 0) or its Experimental page (exper 1): the settings marked exp go on the second */
static void page_settings_list(int exper)
{
    Row r[NS + 3];
    int vis[NS], nv = 0, i, a, deck = !exper && is_steam_deck(), nr;
    int step;
    for (i = 0; i < NS; i++) if (S[i].exp == exper) vis[nv++] = i;
    nr = nv + 1 + deck + !exper;   /* + the Deck preset row (only on a Deck), + Experimental, + Back */
    step = (H - 62 - 186) / (nr > 1 ? nr - 1 : 1);       /* more rows: tighter, Back stays above the footer */
    if (step > 24) step = 24;
    text(&F_ui, 92, 160, RED, 0, exper ? "EXPERIMENTAL" : "PC SETTINGS");
    for (i = 0; i < nv; i++) r[i] = (Row){S[vis[i]].label, 1, vis[i]};
    if (deck) r[nv] = (Row){"Apply Steam Deck preset", 0, A_DECK};
    if (!exper) r[nr - 2] = (Row){"Experimental", 0, A_EXPER};
    r[nr - 1] = (Row){"Back", 0, A_BACK};
    a = rows(r, nr, 92, 186, step, &F_ui, 470, nr);   /* all rows on screen: the values column does not scroll */
    for (i = 0; i < nv; i++) {
        float ry = 186 + i * step;
        int k = vis[i], over = mx >= 440 && mx < 720 && my >= ry - 18 && my < ry + 6;
        text(&F_ui, 470, ry, i == s_sel ? 0xFFFFFFFFu : TXT_DIM, 0, val_str(k));
        if (i == s_sel && !(S[k].lo == 0 && S[k].hi == 1)) {
            text(&F_ui, 450, ry, RED, 1, "<");
            text(&F_ui, 470 + text_w(&F_ui, val_str(k)) + 18, ry, RED, 1, ">");
        }
        if (over) s_hot = 1;
    }
    if (s_sel < nv) {
        if (k_left) change(vis[s_sel], -1);
        if (k_right) change(vis[s_sel], 1);
    }
    if (a >= 0) {
        if (r[a].kind == 1) {
            /* click: on/off flips, numbers step up and wrap */
            int k = r[a].idx;
            if (S[k].lo == 0 && S[k].hi == 1) change(k, 1);
            else if (s_val[k] + S[k].step > S[k].hi) { s_val[k] = S[k].lo; s_ini_dirty = 1; }
            else change(k, 1);
        } else if (r[a].idx == A_DECK) { deck_preset(); msg("Steam Deck preset applied.%s", ""); }
        else if (r[a].idx == A_EXPER) go(EXPER);
        else go(exper ? SETTINGS : HOME);
    }
    if (k_back) go(exper ? SETTINGS : HOME);
    footer("Change", "Back");
    if (s_sel < nv) text(&F_small, W - 40, H - 21, TXT_DIM, 2, S[vis[s_sel]].help);
    else if (deck && s_sel == nv) text(&F_small, W - 40, H - 21, TXT_DIM, 2, "60 fps, 2x, fullscreen, V-Sync, controller prompts, regular renderer.");
    else if (!exper && s_sel == nr - 2) text(&F_small, W - 40, H - 21, TXT_DIM, 2, "Settings still being tested. Turn one off if anything looks wrong.");
}

static void page_settings(void) { page_settings_list(0); }

/* Performance tests (Steam Deck, or CJ_PERF_TESTS=1): one run each, the same save and spot. Each
 * sets its overrides for this launch only (pcj.ini is untouched) and names the run in the log
 * (CJ_PERF_TEST; session folder ...-testN). */
static const struct { const char *label, *about, *tag, *takeover, *persist; } k_tests[] = {
    {"Test 1: Regular renderer", "The regular renderer with the new buffer streaming.", "1", "0", "1"},
    {"Test 2: Regular, old buffers", "The same, with the new buffer streaming off, to compare with test 1.", "2", "0", "0"},
    {"Test 3: Native renderer", "The experimental native renderer.", "3", "1", "1"},
};
static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    SetEnvironmentVariableA(k, v);
#else
    setenv(k, v, 1);
#endif
}
static void act_play(void);
static void page_tests(void)
{
    Row r[5];
    int i, n = 0, a;
    text(&F_ui, 92, 150, RED, 0, "PERFORMANCE TESTS");
    text(&F_small, 92, 178, TXT_DIM, 0, "Same save and spot each time, about two minutes, then quit.");
    for (i = 0; i < 3; i++) r[n++] = (Row){k_tests[i].label, 0, i};
    r[n++] = (Row){"Back", 0, A_BACK};
    a = rows(r, n, 92, 230, 40, &F_big, 0, 4);
    if (a >= 0) {
        if (r[a].idx == A_BACK) go(HOME);
        else {
            i = r[a].idx;
            set_env("CJ_PERF_TEST", k_tests[i].tag);
            set_env("CJ_API_TAKEOVER", k_tests[i].takeover);
            set_env("CJ_GL_PERSIST", k_tests[i].persist);
            set_env("CJ_FPS", "60");
            act_play();
        }
    }
    if (k_back) go(HOME);
    footer("Start test", "Back");
    if (s_sel < 3) text(&F_small, W - 40, H - 21, TXT_DIM, 2, k_tests[s_sel].about);
}

static void page_packs(void)
{
    Row r[100];
    int i, n = 0, a, first_av = -1, want = 0;
    static char lab[100][128];
    int ready = SDL_AtomicGet(&s_av_ready);
    if (ready == 0 && s_repo[0]) {
        SDL_AtomicSet(&s_av_ready, 1);
        SDL_DetachThread(SDL_CreateThread(avail_list, "packs", NULL));
    }
    if (SDL_AtomicCAS(&s_rescan, 1, 0)) packs_scan();
    text(&F_ui, 92, 150, RED, 0, "TEXTURE PACKS");
    for (i = 0; i < s_npack && n < 60; i++) {
        snprintf(lab[n], sizeof lab[n], "%.44s", s_packs[i].name);
        r[n] = (Row){lab[n], 2, i};
        n++;
    }
    if (ready == 2) {
        for (i = 0; i < s_nav && n < 96; i++) {
            if (s_av[i].installed && SDL_AtomicGet(&s_dl_state[i]) != 3) continue;
            if (first_av < 0) { first_av = n; r[n] = (Row){"AVAILABLE TO DOWNLOAD", 4, 0}; n++; }
            snprintf(lab[n], sizeof lab[n], tr("Get  %s"), s_av[i].name);
            r[n] = (Row){lab[n], 3, i};
            n++;
            if (!s_av[i].installed) want++;
        }
        if (want > 1) { r[n] = (Row){"Get all of them", 0, 100}; n++; }
    }
    r[n++] = (Row){"Open the textures folder", 0, A_OPENTEX};
    r[n++] = (Row){"Back", 0, A_BACK};
    a = rows(r, n, 92, 188, 29, &F_ui, 600, 11);
    for (i = s_top; i < n && i < s_top + 11; i++) {
        float ry = 188 + (i - s_top) * 29;
        if (r[i].kind == 2) {
            Pack *p = &s_packs[r[i].idx];
            char sz[32];
            snprintf(sz, sizeof sz, "%.0f MB", p->size / 1048576.0);
            text(&F_small, 580, ry - 2, TXT_DIM, 2, sz);
            text(&F_ui, 610, ry, p->on ? (i == s_sel ? 0xFFFFFFFFu : TXT) : 0xFF6A6460u, 0, p->on ? "On" : "Off");
        } else if (r[i].kind == 3) {
            int k = r[i].idx, st = SDL_AtomicGet(&s_dl_state[k]);
            char sz[48];
            if (st == 2) snprintf(sz, sizeof sz, tr("Downloading %d%%"), SDL_AtomicGet(&s_dl_pct[k]));
            else if (st == 1) snprintf(sz, sizeof sz, "Waiting");
            else if (st == 3) snprintf(sz, sizeof sz, "Installed");
            else if (st == 4) snprintf(sz, sizeof sz, "Failed: try again");
            else snprintf(sz, sizeof sz, "%.0f MB", s_av[k].size / 1048576.0);
            text(&F_small, 580, ry - 2, st == 4 ? 0xFFFF6A60u : (st == 3 ? 0xFF7AD07Au : TXT_DIM), 2, sz);
            if (st == 2) {   /* progress bar under the row */
                fill(92, ry + 6, 488, 3, 0x40FFFFFFu);
                fill(92, ry + 6, 488.0f * SDL_AtomicGet(&s_dl_pct[k]) / 100, 3, RED);
            }
        }
    }
    if (ready == 1) text(&F_small, 92, 172, TXT_DIM, 0, "Looking for packs to download...");
    else if (s_npack == 0 && first_av < 0) text(&F_small, 92, 172, TXT_DIM, 0, "No packs yet.");
    if (a >= 0) {
        if (r[a].kind == 2) pack_toggle(r[a].idx);
        else if (r[a].kind == 3) { if (SDL_AtomicGet(&s_dl_state[r[a].idx]) == 4) SDL_AtomicSet(&s_dl_state[r[a].idx], 0); dl_queue(r[a].idx); }
        else if (r[a].idx == 100) { for (i = 0; i < s_nav; i++) dl_queue(i); }
        else if (r[a].idx == A_OPENTEX) {
#ifdef _WIN32
            ShellExecuteA(NULL, "open", s_tex, NULL, NULL, SW_SHOWNORMAL);
#else
            open_with_system(s_tex);
#endif
        } else go(HOME);
    }
    if (s_sel < n && r[s_sel].kind == 2 && (k_left || k_right)) pack_toggle(r[s_sel].idx);
    if (k_back) go(HOME);
    if (s_sel < n && r[s_sel].kind == 3) {
        footer("Download", "Back");
        text(&F_small, W - 40, H - 21, TXT_DIM, 2, s_av[r[s_sel].idx].about);
    } else {
        footer("Switch on / off", "Back");
        text(&F_small, W - 40, H - 21, TXT_DIM, 2, "Drop a texture mod or pack on this window to add it.");
    }
}

/* input: called before the page (a click closes a finished box); else draws it over the page */
static void drop_box(int input)
{
    int st = SDL_AtomicGet(&s_conv), pct = SDL_AtomicGet(&s_conv_pct);
    char line[256];
    static Uint32 shown;
    float x = 92, y = H - 140, w = W - 184;
    if (!st) return;
    if (st >= 2 && !shown) shown = SDL_GetTicks();
    if (st >= 2 && (SDL_GetTicks() - shown > 20000 || (input && (mclick || k_ok || k_back)))) {
        SDL_AtomicSet(&s_conv, 0);
        shown = 0;
        mclick = k_ok = k_back = 0;   /* the click closes the box and does nothing else */
        return;
    }
    if (input) return;
    SDL_LockMutex(s_conv_mx);
    snprintf(line, sizeof line, "%s", s_conv_line);
    SDL_UnlockMutex(s_conv_mx);
    fill(x, y, w, 60, 0xE0101010u);
    frame(x, y, w, 60, st == 3 ? 0xFFFF6A60u : RED);
    text(&F_small, x + 16, y + 22, st == 3 ? 0xFFFF6A60u : (st == 2 ? 0xFF7AD07Au : TXT), 0,
         st == 1 ? "ADDING A MOD" : st == 2 ? "DONE: packs apply the next time you play" : "COULDN'T ADD THAT");
    text(&F_small, x + 16, y + 44, TXT_DIM, 0, line);
    if (st == 1 && pct >= 0) {
        fill(x + 16, y + 52, w - 32, 3, 0x40FFFFFFu);
        fill(x + 16, y + 52, (w - 32) * pct / 100.0f, 3, RED);
    }
}

/* ---------------------------------------------------------------- main */
static unsigned char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(b = malloc((size_t)n))) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static unsigned char *load_font(void)
{
    const char *env = getenv("CJ_LAUNCHER_FONT");
    static const char *cand[] = {"bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf", "segoeui.ttf", "arial.ttf"};
    char p[1200], win[600] = "C:\\Windows";
    unsigned char *b;
    size_t i;
    if (env && *env && (b = read_all(env))) return b;
#ifdef _WIN32
    GetWindowsDirectoryA(win, sizeof win);
#endif
    for (i = 0; i < sizeof cand / sizeof *cand; i++) {
        snprintf(p, sizeof p, "%s\\Fonts\\%s", win, cand[i]);
        if ((b = read_all(p))) return b;
    }
    {   /* Proton / Wine (a prefix may have no Windows fonts; the Linux root is Z:) and Linux: the
         * fonts SteamOS and the common distributions ship */
        static const char *lin[] = {
            "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/opentype/noto/NotoSans-Bold.ttf",
        };
        for (i = 0; i < sizeof lin / sizeof *lin; i++) {
#ifdef _WIN32
            size_t k;
            snprintf(p, sizeof p, "Z:%s", lin[i]);
            for (k = 0; p[k]; k++) if (p[k] == '/') p[k] = '\\';
#else
            snprintf(p, sizeof p, "%s", lin[i]);
#endif
            if ((b = read_all(p))) return b;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    SDL_Window *win;
    SDL_GameController *pad = NULL;
    unsigned char *ttf;
    Uint32 t0;
    int w;
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0 && SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    find_paths();
    ini_load();
    deck_first_run();
    {   /* the installed version, before the network check answers */
        char vp[1200];
        FILE *f;
        snprintf(vp, sizeof vp, "%s" SEP "VERSION", s_repo);
        if (s_repo[0] && (f = fopen(vp, "r"))) {
            if (fgets(s_have, sizeof s_have, f)) s_have[strcspn(s_have, "\r\n")] = 0;
            fclose(f);
        }
    }
    win = SDL_CreateWindow("Predator: Concrete Jungle PC", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                           SDL_WINDOW_ALLOW_HIGHDPI);
    if (win) {   /* window icon: the clan glyph made by tools/launcher_art.py from the player's disc */
        char ip[1200];
        int iw, ih, ic;
        unsigned char *ipx;
        snprintf(ip, sizeof ip, "%s" SEP "launcher-art" SEP "icon.png", s_dir);
        if ((ipx = stbi_load(ip, &iw, &ih, &ic, 4))) {
            SDL_Surface *is = SDL_CreateRGBSurfaceFrom(ipx, iw, ih, 32, iw * 4, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
            if (is) { SDL_SetWindowIcon(win, is); SDL_FreeSurface(is); }
            stbi_image_free(ipx);
        }
    }
    R = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!R && win) R = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!R) return 1;
    SDL_RenderSetLogicalSize(R, W, H);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (!(ttf = load_font())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launcher", "No usable font found (Windows Fonts folder or /usr/share/fonts).", win);
        return 1;
    }
    font_bake(&F_title, ttf, 64);
    font_bake(&F_big, ttf, 30);
    font_bake(&F_ui, ttf, 21);
    font_bake(&F_small, ttf, 15);
    s_bg = load_png("bg.png", &w, &w);
    s_logo = load_png("logo.png", &s_logo_w, &s_logo_h);
    SDL_DetachThread(SDL_CreateThread(update_check, "update", NULL));
    t0 = SDL_GetTicks();
    while (!s_quit) {
        SDL_Event e;
        mclick = mmoved = 0;
        k_up = k_down = k_left = k_right = k_ok = k_back = k_lang = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: s_quit = 1; break;
            case SDL_MOUSEMOTION: mx = e.motion.x; my = e.motion.y; mmoved = 1; break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) { mx = e.button.x; my = e.button.y; mclick = 1; }
                else if (e.button.button == SDL_BUTTON_RIGHT) k_back = 1;
                break;
            case SDL_DROPFILE:
                if (s_ndrop < MAXDROP) s_drop[s_ndrop++] = e.drop.file;
                else SDL_free(e.drop.file);
                break;
            case SDL_DROPCOMPLETE: drop_go(); if (s_page != PACKS) go(PACKS); break;
            case SDL_MOUSEWHEEL: if (e.wheel.y > 0) k_up = 1; else if (e.wheel.y < 0) k_down = 1; break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_UP: case SDLK_w: k_up = 1; break;
                case SDLK_DOWN: case SDLK_s: k_down = 1; break;
                case SDLK_LEFT: case SDLK_a: k_left = 1; break;
                case SDLK_RIGHT: case SDLK_d: k_right = 1; break;
                case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: k_ok = 1; break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: k_back = 1; break;
                case SDLK_l: k_lang = 1; break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: k_up = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k_down = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k_left = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k_right = 1; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: k_ok = 1; break;
                case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: k_back = 1; break;
                case SDL_CONTROLLER_BUTTON_Y: k_lang = 1; break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION: {   /* left stick as a d-pad, one step per push */
                static int ly, lx;
                int v = e.caxis.value > 20000 ? 1 : e.caxis.value < -20000 ? -1 : 0;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY && v != ly) { if (v < 0) k_up = 1; else if (v > 0) k_down = 1; ly = v; }
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX && v != lx) { if (v < 0) k_left = 1; else if (v > 0) k_right = 1; lx = v; }
                break;
            }
            }
        }
        s_hot = 0;
        {   /* the launcher's own language follows the Language setting */
            int k;
            for (k = 0; k < NS; k++) if (!strcmp(S[k].key, "CJ_LANG")) s_ui_pt = s_val[k] == 1;
        }
        drop_box(1);
        background(SDL_GetTicks() - t0, s_page != HOME);
        if (s_page == HOME) page_home();
        else if (s_page == SETTINGS) page_settings();
        else if (s_page == TESTS) page_tests();
        else if (s_page == EXPER) page_settings_list(1);
        else page_packs();
        drop_box(0);
        if (s_msg && SDL_GetTicks() - s_msg_t < 8000) {
            fill(0, H - 72, W, 26, 0xC0000000u);
            text(&F_small, 52, H - 54, 0xFFFFB0A0u, 0, s_msg);
        }
        {
            static SDL_Cursor *hand, *arrow;
            if (!hand) { hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND); arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW); }
            SDL_SetCursor(s_hot ? hand : arrow);
        }
        SDL_RenderPresent(R);
    }
    if (s_ini_dirty) ini_save();
    SDL_Quit();
#ifndef _WIN32
    if (s_play_path[0]) {
        if (chdir(s_dir) != 0) return 1;
        execl(s_play_path, s_play_path, "--game", (char *)NULL);   /* PCJ-Linux.sh --game: straight to the game */
        execl("/bin/bash", "bash", s_play_path, "--game", (char *)NULL);   /* lost its executable bit (USB copy) */
        return 1;
    }
#endif
    return 0;
}
