/* Host-side tests for the title cleaner. Built and run by CI on Linux:
   cc -O1 -I src tests/test_parse.c src/parse.c -o t && ./t */
#include <stdio.h>
#include <string.h>

#include "parse.h"

static int fails = 0, total = 0;

static void file_case(const char *name, const char *title, int season, int ep, const char *ep_title, int year) {
    ParsedName p;
    parse_filename(name, &p);
    total++;
    int ok = !strcmp(p.title, title) && p.season == season && p.episode == ep &&
             (!ep_title || !strcmp(p.ep_title, ep_title)) && (year < 0 || p.year == year);
    if (!ok) {
        fails++;
        printf("FAIL file  %-60s -> '%s' S%d E%d '%s' y%d  (want '%s' S%d E%d '%s' y%d)\n", name, p.title,
               p.season, p.episode, p.ep_title, p.year, title, season, ep, ep_title ? ep_title : "*", year);
    }
}

static void clean_case(const char *name, const char *title, int year) {
    char out[256];
    int y = 0, s = -1;
    parse_clean(name, out, sizeof(out), &y, &s);
    total++;
    if (strcmp(out, title) || (year >= 0 && y != year)) {
        fails++;
        printf("FAIL clean %-60s -> '%s' y%d  (want '%s' y%d)\n", name, out, y, title, year);
    }
}

static void season_case(const char *dir, int want) {
    int s = parse_season_folder(dir);
    total++;
    if (s != want) {
        fails++;
        printf("FAIL season %-50s -> %d (want %d)\n", dir, s, want);
    }
}

static void book_case(const char *name, const char *title, int vol) {
    ParsedName p;
    parse_book(name, &p);
    total++;
    if (strcmp(p.title, title) || p.volume != vol) {
        fails++;
        printf("FAIL book %-50s -> '%s' v%d (want '%s' v%d)\n", name, p.title, p.volume, title, vol);
    }
}

int main(void) {
    /* the user's own examples */
    file_case("Shameless.S02E04.1080p.BluRay.x265.DDP5.1.mkv", "Shameless", 2, 4, "", -1);
    clean_case("shameless [1080p] [webrip] [aac] [x265] [mkv]", "Shameless", 0);
    clean_case("Shameless.S02.1080p.BluRay.x265-RARBG", "Shameless", -1);
    file_case("shameless.s01e01.720p.webrip.aac.mkv", "Shameless", 1, 1, "", -1);

    /* episodes */
    file_case("The.Office.US.S03E12.Traveling.Salesmen.720p.WEB-DL.DD5.1.H.264.mkv", "The Office US", 3, 12,
              "Traveling Salesmen", -1);
    file_case("Breaking Bad - S05E14 - Ozymandias (1080p BluRay x265 10bit).mkv", "Breaking Bad", 5, 14,
              "Ozymandias", -1);
    file_case("Doctor.Who.2005.S01E01.Rose.1080p.mkv", "Doctor Who", 1, 1, "Rose", 2005);
    file_case("friends.1x05.the.one.with.the.east.german.laundry.detergent.avi", "Friends", 1, 5, NULL, -1);
    file_case("S01E03.mkv", "", 1, 3, "", -1);
    file_case("Episode 7.mp4", "", -1, 7, NULL, -1);
    file_case("[SubsPlease] Frieren - 05 (1080p) [ABCD1234].mkv", "Frieren", -1, 5, NULL, -1);
    file_case("Mr.Robot.S02E01E02.1080p.mkv", "Mr Robot", 2, 1, NULL, -1);
    file_case("Stranger Things S04 E09 Chapter Nine The Piggyback 2160p NF WEB-DL DDP5.1 Atmos DV HDR H.265.mkv",
              "Stranger Things", 4, 9, "Chapter Nine The Piggyback", -1);
    file_case("05 - Pilot.mp4", "", -1, 5, "Pilot", -1);
    file_case("Naruto 001.mkv", "Naruto", -1, 1, NULL, -1);
    file_case("Shameless (US) S03E02 [WEBRip 720p].mp4", "Shameless (US)", 3, 2, NULL, -1);
    file_case("Season 2 Episode 3.mp4", "", 2, 3, NULL, -1);

    /* movies */
    file_case("Blade.Runner.2049.2017.2160p.UHD.BluRay.x265.mkv", "Blade Runner 2049", -1, -1, NULL, 2017);
    file_case("1917.2019.1080p.BluRay.x264.mkv", "1917", -1, -1, NULL, 2019);
    file_case("Charlotte's.Web.1973.720p.mp4", "Charlotte's Web", -1, -1, NULL, 1973);
    file_case("The Matrix (1999) [1080p] [BluRay] [5.1] [YTS.MX].mp4", "The Matrix", -1, -1, NULL, 1999);
    file_case("Spider-Man.Into.the.Spider-Verse.2018.WEBRip.x264-ION10.mp4", "Spider-Man Into the Spider-Verse",
              -1, -1, NULL, 2018);
    file_case("Dune Part Two 2024 1080p WEB-DL HEVC x265 5.1 BONE.mkv", "Dune Part Two", -1, -1, NULL, 2024);
    file_case("inception.mp4", "Inception", -1, -1, NULL, 0);
    file_case("Oppenheimer.2023.IMAX.2160p.WEB.H265-GROUP.mkv", "Oppenheimer", -1, -1, NULL, 2023);
    file_case("Avatar.The.Way.of.Water.2022.EXTENDED.1080p.WEBRip.mp4", "Avatar The Way of Water", -1, -1, NULL, 2022);
    file_case("2012.2009.BRRip.XviD.avi", "2012", -1, -1, NULL, 2009);
    clean_case("Interstellar.2014.REMASTERED.1080p.BluRay.10bit.HDR.DDP5.1.Atmos-GROUP", "Interstellar", 2014);
    clean_case("The Lord of the Rings The Return of the King (2003) Extended BR-Rip", "The Lord of the Rings The Return of the King", 2003);

    file_case("House.of.the.Dragon.S02E01.WEB.H264-GROUP.mkv", "House of the Dragon", 2, 1, "", -1);
    file_case("The.4400.S01E01.Pilot.mkv", "The 4400", 1, 1, "Pilot", -1);
    file_case("www.Torrenting.com   -   Shameless.S05E02.720p.mkv", "Shameless", 5, 2, "", -1);
    file_case("Avengers Endgame (2019) 1080p.mp4", "Avengers Endgame", -1, -1, NULL, 2019);
    file_case("The Office - 2x01 - The Dundies.mp4", "The Office", 2, 1, "The Dundies", -1);
    file_case("[Erai-raws] Jujutsu Kaisen - 24 [1080p][Multiple Subtitle].mkv", "Jujutsu Kaisen", -1, 24, NULL, -1);

    /* the user's real converted library */
    file_case("Clarence E01E02 Fun Dungeon Face + Pretty Great Day with a Girl.mp4", "Clarence", -1, 1,
              "Fun Dungeon Face + Pretty Great Day with a Girl", -1);
    file_case("Clarence Episode 27 Nothing Ventured.mp4", "Clarence", -1, 27, "Nothing Ventured", -1);
    file_case("Shameless.US.S10E01.1080p.AMZN.WEB-DL.DDP5.1.H.264-NTb.mp4", "Shameless US", 10, 1, "", -1);
    file_case("Shameless.US.S11E01.1080p.WEBRip.AC3.x264-LESS.mp4", "Shameless US", 11, 1, "", -1);
    season_case("Clarence 2014 Season 1 Complete WEB-DL x264 [i_c]", 1);
    season_case("Shameless.US.S10.1080p.AMZN.WEBRip.DDP5.1.x264-NTb[rartv]", 10);
    season_case("Shameless.US.Season.5.S05.1080p.Bluray.x265.HEVC.5.1.Complete[fs87]", 5);
    season_case("Shameless.US.S11.COMPLETE.1080p.WEBRip.AC3.x264-LESS", 11);

    /* folders */
    season_case("Season 1", 1);
    season_case("season 02", 2);
    season_case("S03", 3);
    season_case("Specials", 0);
    season_case("Shameless.S02.1080p.BluRay.x265", 2);
    season_case("Series 4", 4);
    season_case("Shameless", -1);
    season_case("Inception (2010)", -1);

    /* books */
    book_case("Berserk v01 (Digital) (danke-Empire).cbz", "Berserk", 1);
    book_case("One Piece - Vol. 12.cbz", "One Piece", 12);
    book_case("Chainsaw Man Chapter 097.cbr", "Chainsaw Man", 97);
    book_case("The Hobbit.epub", "The Hobbit", -1);

    printf("%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
