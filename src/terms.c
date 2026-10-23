/* terms.c -- what the features are called in each theme: the LCARS bridge, the classic light and dark themes (plain
 * names), Stardew Valley's farm, a Pokémon adventure and Stark's JARVIS. The feature is the same; only its name changes. */
#include <string.h>
#include "app.h"

static const char *const TERMS[T__N][14] = {
    /*  LCARS / classic / Stardew / Pokemon / JARVIS / Severance / Alien / Pip-Boy / NERV / Portal / Dark Souls / Factorio / Windows 95 / Star Citizen */
    [T_SWEEP]     = { "Sensor sweep", "Pre-run check", "Morning field check", "Pokédex scan", "Running diagnostics",
                      "Refinement check", "System diagnostic", "V.A.T.S. scan", "MAGI deliberation", "Test protocol review",
                      "Inspect the path ahead", "Production check", "ScanDisk", "Pre-flight check" },
    [T_ALERT]     = { "Red alert", "Divergence warning", "Storm warning", "Your run is low on HP", "Critical failure",
                      "Macrodata breach", "Special Order 937", "Radiation critical", "Pattern blue", "Test failure imminent",
                      "You died", "Biters attacking", "General protection fault", "Hull integrity critical" },
    [T_WATCH]     = { "Yellow alert", "Divergence watch", "Clouds gathering", "Your run looks hurt", "Anomaly detected",
                      "The numbers feel scary", "Anomaly detected", "Rads rising", "Pattern orange", "Unusual test behaviour",
                      "Something stirs in the dark", "Pollution spreading", "Not responding", "Shields failing" },
    [T_PALETTE]   = { "Computer", "Command palette", "Journal", "PC", "JARVIS",
                      "Lumon terminal", "MU/TH/UR", "Pip-Boy", "MAGI", "Facility AI", "Messages", "Console", "Run...", "mobiGlas" },
    [T_REVIEW]    = { "Make it so", "Review changes", "Shipping bin", "Save the game?", "Awaiting your approval",
                      "Review with management", "Confirm order", "Vault-Tec release form", "MAGI vote", "Sign the release form",
                      "Review the covenant", "Blueprint preview", "Are you sure?", "Review the contract" },
    [T_REVIEW_GO] = { "Make it so", "Save changes", "Ship it", "Save", "Approve",
                      "Submit to the board", "Execute", "Accept", "Approve (3 of 3)", "Proceed to testing", "Rest at the bonfire",
                      "Build", "OK", "Accept contract" },
    [T_EXPLAIN]   = { "Diagnostic", "What went wrong", "The Wizard's reading", "Professor's note", "JARVIS analysis",
                      "Wellness session", "MU/TH/UR analysis", "Diagnosis", "MAGI analysis", "Test results",
                      "A message from another world", "Alert details", "Details", "Ship diagnostics" },
    [T_RUN_GO]    = { "Engage", "Run", "Get to work on", "Go,", "Initiate",
                      "Begin refinement", "Initiate", "Engage", "Launch", "Commence testing", "Link the fire", "Start production",
                      "Start", "Quantum jump" },
    [T_ASK]       = { "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude",
                      "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude", "Ask Claude" },
    [T_LOG]       = { "Captain's log", "Run records", "Farm diary", "Trainer's journal", "Flight log",
                      "Refinement records", "Mother's log", "Holotapes", "Operation records", "Test records", "Bonfire journal",
                      "Production statistics", "Event Viewer", "Contract history" },
    [T_AWAY]      = { "Away mission", "Prepare for the cluster", "Pack for the mines", "Trade to another Trainer", "Remote deployment",
                      "Send to the outie", "Ship to orbit", "Leave the vault", "Deploy to Matsushiro", "Off-site testing",
                      "Journey to another land", "Launch a rocket", "Map network drive", "Jump to another system" },
};

static int col_of(int theme)
{
    switch (theme) {
    case THEME_LCARS: return 0;
    case THEME_STARDEW: return 2;
    case THEME_POKEMON: return 3;
    case THEME_JARVIS: return 4;
    default: return theme >= THEME_SEVERANCE && theme <= THEME_STARCITIZEN ? 5 + (theme - THEME_SEVERANCE) : 1;
    }
}

const char *term(Term t) { return t >= 0 && t < T__N ? TERMS[t][col_of(A.set.theme)] : ""; }
const char *term_in(Term t, int theme) { return t >= 0 && t < T__N ? TERMS[t][col_of(theme)] : ""; }

/* the RED ALERT card's title in the theme's words: "<name>: <what>" (redalert.c writes the LCARS one) */
const char *alert_title(const Alert *a)
{
    static char b[160];
    const char *colon = strchr(a->title, ':');
    pl_fmt_ui(b, sizeof b, "%s%s", term(a->level == RA_ALERT ? T_ALERT : T_WATCH), colon ? colon : "");
    return b;
}
