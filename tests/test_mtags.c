/* file-name guessing for songs without tags */
#include "mtags.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *path, const char *title, const char *artist, const char *album, int track, int disc) {
    MTags t;
    memset(&t, 0, sizeof(t));
    mtags_guess(path, &t);
    int ok = !strcmp(t.title, title) && !strcmp(t.artist, artist) && !strcmp(t.album, album) && t.track == track && t.disc == disc;
    if (!ok) fails++;
    printf("%s %-70s -> [%s] [%s] [%s] %d/%d\n", ok ? "ok  " : "FAIL", path, t.title, t.artist, t.album, t.track, t.disc);
}

int main(void) {
    check("ux0:music/MF DOOM/Madvillainy (2004)/03 - Accordion.mp3", "Accordion", "MF DOOM", "Madvillainy", 3, 0);
    check("ux0:music/Madvillain - Madvillainy (2004) [320kbps]/01. The Illest Villains.mp3", "The Illest Villains", "Madvillain", "Madvillainy", 1, 0);
    check("ux0:music/Nujabes - Modal Soul/1-04 Feather (feat. Cise Starr & Akin).flac", "Feather (feat. Cise Starr & Akin)", "Nujabes", "Modal Soul", 4, 1);
    check("ux0:music/A Tribe Called Quest/Midnight Marauders/CD1/05 Electric Relaxation.mp3", "Electric Relaxation", "A Tribe Called Quest", "Midnight Marauders", 5, 1);
    check("ux0:music/Downloads/Ark Patrol - Let Go (Official Audio).mp3", "Let Go", "Ark Patrol", "", 0, 0);
    check("ux0:music/Smashing Pumpkins/Mellon Collie/1979.mp3", "1979", "Smashing Pumpkins", "Mellon Collie", 0, 0);
    check("ux0:music/Gang_Starr_-_Moment_Of_Truth-1998-FTD/04-gang_starr-you_know_my_steez.mp3", "You Know My Steez", "Gang Starr", "Moment Of Truth", 4, 0);
    check("ux0:music/The Pharcyde - Bizarre Ride II/Track 07.mp3", "Track 07", "The Pharcyde", "Bizarre Ride II", 7, 0);
    check("ux0:music/2004 - Madvillainy/12 - All Caps.ogg", "All Caps", "", "Madvillainy", 12, 0);
    check("uma0:Music/www.mp3site.com - Pharcyde - Passin Me By [HQ].mp3", "Passin Me By", "Pharcyde", "", 0, 0);
    check("ux0:music/Artist - Album/101 - First.mp3", "First", "Artist", "Album", 1, 1);
    check("ux0:music/22 Acacia Avenue.mp3", "Acacia Avenue", "", "", 22, 0);
    printf("%d failed\n", fails);
    return fails != 0;
}
