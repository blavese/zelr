/* The sites read another way (sites.h), against pages of their shape.
 *
 * What YouTube and Twitch send changes when they like, so nothing here is a
 * copy of what they sent once. These are pages written to the shapes the
 * reader looks for -- a search, a watch page, a channel, the API's answers
 * -- with the awkward parts put in on purpose: escaped quotes, keys written
 * inside strings, characters past the first sixty five thousand, ids that
 * would close the link they are written into, names that would close the
 * query, a video listed twice and a playlist among the videos.
 *
 * Whether the real sites still have those shapes is tools/sitecheck.py's
 * question, asked of the real sites. This one is whether the reader does
 * what it should with them, and it is asked with no network at all.
 *
 *   sitetest              the checks
 *   sitetest live URL     reads one real page and says what it found
 */
#include "zelr.h"
#include "alloc.h"
#include "sites.h"
#include "ui.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static void okn(const char *what, int cond, int n) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  ");
    putn(n);
    putc('\n');
    if (!cond) failed++;
}

static int has(const char *s, const char *what) {
    return site_search(s, 0, w_len(s), what) >= 0;
}

static int count(const char *s, const char *what) {
    int n = 0, len = w_len(s), wl = w_len(what);
    for (int at = site_search(s, 0, len, what); at >= 0; at = site_search(s, at + wl, len, what)) n++;
    return n;
}

static int at_url(const char *text, url_t *u) {
    return url_parse(text, u);
}

static char page[64 * 1024];

static int yt(const char *address, const char *data) {
    url_t u;
    if (!at_url(address, &u)) return -1;
    return site_youtube(&u, data, w_len(data), page, (int)sizeof(page));
}

/* --- pages of the shapes YouTube sends ---------------------------------------- */

static const char YT_SEARCH[] =
    "<!DOCTYPE html><html><head><title>YouTube</title></head><body><script>"
    "var ytInitialData = {\"contents\":{\"sectionListRenderer\":{\"contents\":[{\"itemSectionRenderer\":{\"contents\":["
    "{\"videoRenderer\":{\"videoId\":\"AAAAAAAAAA1\","
    "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/AAAAAAAAAA1/hq720.jpg\",\"width\":720}]},"
    "\"title\":{\"runs\":[{\"text\":\"Cats \"},{\"text\":\"\\\"and\\\" dogs \\u0026 more\"}],"
    "\"accessibility\":{\"accessibilityData\":{\"label\":\"not this\"}}},"
    "\"longBylineText\":{\"runs\":[{\"text\":\"Animal Channel\",\"navigationEndpoint\":{\"browseEndpoint\":"
    "{\"browseId\":\"UC1\",\"canonicalBaseUrl\":\"/@animals\"}}}]},"
    "\"publishedTimeText\":{\"simpleText\":\"2 days ago\"},"
    "\"lengthText\":{\"accessibility\":{\"accessibilityData\":{\"label\":\"3 minutes\"}},\"simpleText\":\"3:21\"},"
    "\"viewCountText\":{\"simpleText\":\"1,234 views\"},"
    "\"detailedMetadataSnippets\":[{\"snippetText\":{\"runs\":[{\"text\":\"A line about \"},{\"text\":\"cats\",\"bold\":true}]}}]}},"
    "{\"videoRenderer\":{\"videoId\":\"BBBBBBBBBB2\",\"title\":{\"runs\":[{\"text\":\"Smile \\ud83d\\ude00 <script>\"}]},"
    "\"ownerText\":{\"runs\":[{\"text\":\"Second\",\"navigationEndpoint\":{\"browseEndpoint\":"
    "{\"browseId\":\"UC2second\",\"canonicalBaseUrl\":\"/@x\\\"><script>\"}}}]},"
    "\"viewCountText\":{\"simpleText\":\"12 views\"}}},"
    "{\"videoRenderer\":{\"videoId\":\"AAAAAAAAAA1\",\"title\":{\"runs\":[{\"text\":\"the same again\"}]}}},"
    "{\"videoRenderer\":{\"videoId\":\"\\\"><img src=x>\",\"title\":{\"runs\":[{\"text\":\"a bad id\"}]}}},"
    "{\"videoRenderer\":{\"videoId\":\"short\",\"title\":{\"runs\":[{\"text\":\"too short\"}]}}},"
    "{\"channelRenderer\":{\"title\":{\"simpleText\":\"\\\"videoRenderer\\\":{\\\"videoId\\\":\\\"FAKEFAKEFAK\\\"}\"}}}"
    "]}}]}}};</script></body></html>";

static const char YT_WATCH[] =
    "<html><body><script>var ytInitialPlayerResponse = {\"playabilityStatus\":{\"status\":\"OK\"},"
    "\"streamingData\":{\"formats\":[{\"itag\":18}]},"
    "\"videoDetails\":{\"videoId\":\"CCCCCCCCCC3\",\"title\":\"A <b>bold</b> title\",\"lengthSeconds\":\"3725\","
    "\"keywords\":[\"x\"],\"channelId\":\"UC3\","
    "\"shortDescription\":\"line one\\nline two \\u2192 https://example.com/?a=1\\u0026b=2\","
    "\"isCrawlable\":true,\"thumbnail\":{\"thumbnails\":[]},\"viewCount\":\"1234567\",\"author\":\"Maker\","
    "\"isLiveContent\":true,\"isLive\":false},"
    "\"storyboards\":{\"playerStoryboardSpecRenderer\":{\"spec\":"
    "\"https://i.ytimg.com/sb/CCCCCCCCCC3/storyboard3_L$L/$N.jpg?sqp=abc"
    "|48#27#100#10#10#0#default#rs$AAA|80#45#60#10#10#2000#M$M#rs$BBB"
    "|160#90#60#5#5#2000#M$M#rs$CCC|320#180#60#3#3#2000#M$M#rs$DDD\"}}};</script>"
    "<script>var ytInitialData = {\"contents\":{\"twoColumnWatchNextResults\":{\"secondaryResults\":"
    "{\"secondaryResults\":{\"results\":["
    "{\"lockupViewModel\":{\"contentImage\":{},\"metadata\":{\"lockupMetadataViewModel\":"
    "{\"title\":{\"content\":\"Related \\\"one\\\"\"},\"metadata\":{\"contentMetadataViewModel\":{\"metadataRows\":["
    "{\"metadataParts\":[{\"text\":{\"content\":\"Other Maker\"}}]},"
    "{\"metadataParts\":[{\"text\":{\"content\":\"5K views\"}},{\"text\":{\"content\":\"1 year ago\"}}]}]}}}},"
    "\"contentId\":\"DDDDDDDDDD4\",\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\"}},"
    "{\"lockupViewModel\":{\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"A playlist\"}}},"
    "\"contentId\":\"PLxxxxxxxxxxxxxxxx\",\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\"}},"
    "{\"compactVideoRenderer\":{\"videoId\":\"EEEEEEEEEE5\",\"title\":{\"simpleText\":\"Older kind\"},"
    "\"longBylineText\":{\"runs\":[{\"text\":\"Old Maker\"}]},\"viewCountText\":{\"simpleText\":\"9 views\"}}}"
    "]}}}}};</script></body></html>";

static const char YT_LIVE[] =
    "<script>var ytInitialPlayerResponse = {\"videoDetails\":{\"videoId\":\"GGGGGGGGGG7\",\"title\":\"On air\","
    "\"lengthSeconds\":\"0\",\"author\":\"Station\",\"viewCount\":\"10\",\"isLive\":true},"
    "\"storyboards\":{\"playerStoryboardSpecRenderer\":{\"spec\":"
    "\"https://i.ytimg.com/sb/GGGGGGGGGG7/storyboard3_L$L/$N.jpg?sqp=x|160#90#60#5#5#2000#M$M#rs$G\"}}};</script>";

/* Storyboards whose address is somewhere other than YouTube's picture
   server: the address is written into the page, so it is not used. */
static const char YT_ELSEWHERE[] =
    "<script>var ytInitialPlayerResponse = {\"videoDetails\":{\"videoId\":\"HHHHHHHHHH8\",\"title\":\"Else\","
    "\"lengthSeconds\":\"60\",\"author\":\"Where\",\"viewCount\":\"1\",\"isLive\":false},"
    "\"storyboards\":{\"playerStoryboardSpecRenderer\":{\"spec\":"
    "\"https://evil.example/sb/HHHHHHHHHH8/L$L/$N.jpg?x|160#90#30#5#5#2000#M$M#rs$H\"}}};</script>";

static const char YT_CHANNEL[] =
    "<script>var ytInitialData = {\"metadata\":{\"channelMetadataRenderer\":{\"title\":\"Some \\u0026 One\","
    "\"description\":\"About us.\",\"externalId\":\"UCnnnnnnnnnnnnnnnnnnnnnn\"}},\"contents\":{\"tabs\":[{\"richGridRenderer\":{\"contents\":["
    "{\"lockupViewModel\":{\"contentId\":\"FFFFFFFFFF6\",\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\","
    "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Upload\"},\"metadata\":"
    "{\"contentMetadataViewModel\":{\"metadataRows\":[{\"metadataParts\":[{\"text\":{\"content\":\"3 views\"}},"
    "{\"text\":{\"content\":\"1 day ago\"}}]}]}}}}}}]}}]}};</script>";

static const char YT_FRONT[] = "<script>var ytInitialData = {\"contents\":{}};</script>";

/* A watch page's comments section, with its token and the client it names,
   and the API's answer to it. */
/* Another section with a token of its own comes first, so taking the right
   one is a choice. */
static const char YT_WITH_COMMENTS[] =
    "<script>ytcfg.set({\"INNERTUBE_CLIENT_VERSION\":\"2.20260925.08.00\"});</script>"
    "<script>var ytInitialData = {\"contents\":{\"results\":["
    "{\"itemSectionRenderer\":{\"contents\":[{\"continuationItemRenderer\":{\"continuationEndpoint\":"
    "{\"continuationCommand\":{\"token\":\"NOTTHISONE\"}}}}],\"sectionIdentifier\":\"other\"}},"
    "{\"itemSectionRenderer\":{\"contents\":["
    "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":"
    "{\"token\":\"Eg0SC2RR-_x%3D\",\"request\":\"CONTINUATION_REQUEST_TYPE_WATCH_NEXT\"}}}}],"
    "\"sectionIdentifier\":\"comment-item-section\"}}]}};</script>";

static const char YT_BAD_TOKEN[] =
    "<script>var ytInitialData = {\"contents\":{\"results\":[{\"itemSectionRenderer\":{\"contents\":["
    "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":"
    "{\"token\":\"abc\\\",\\\"x\\\":\\\"y\"}}}}],\"sectionIdentifier\":\"comment-item-section\"}}]}};</script>";

static const char YT_NO_COMMENTS[] =
    "<script>var ytInitialData = {\"contents\":{\"results\":[{\"itemSectionRenderer\":{\"contents\":"
    "[{\"messageRenderer\":{\"text\":{\"simpleText\":\"Comments are turned off.\"}}}],"
    "\"sectionIdentifier\":\"comment-item-section\"}}]}};</script>";

static const char YT_COMMENTS_ANSWER[] =
    "{\"frameworkUpdates\":{\"entityBatchUpdate\":{\"mutations\":["
    "{\"payload\":{\"commentEntityPayload\":{\"properties\":{\"commentId\":\"a\","
    "\"content\":{\"content\":\"never <gave> us up\\nsecond line\"},\"publishedTime\":\"1 year ago\"},"
    "\"author\":{\"displayName\":\"@YouTube\",\"channelId\":\"UC1\"},"
    "\"toolbar\":{\"likeCountNotliked\":\"321K\",\"replyCount\":\"963\"}}}},"
    "{\"payload\":{\"commentEntityPayload\":{\"properties\":{\"content\":{\"content\":\"one\"},"
    "\"publishedTime\":\"2 days ago\"},\"author\":{\"displayName\":\"@b\"},"
    "\"toolbar\":{\"likeCountNotliked\":\"1\",\"replyCount\":\"0\"}}}},"
    "{\"payload\":{\"commentEntityPayload\":{\"properties\":{\"publishedTime\":\"now\"},"
    "\"author\":{\"displayName\":\"@empty\"}}}}"
    "]}}}";

/* A Short and a playlist in a search, and a playlist's own page. */
static const char YT_MIXED[] =
    "<script>var ytInitialData = {\"contents\":["
    "{\"shortsLockupViewModel\":{\"entityId\":\"shorts-shelf-item-SSSSSSSSSS1\","
    "\"accessibilityText\":\"Tiny <tune>, 1.3 million views \\u2013 play Short\","
    "\"onTap\":{\"innertubeCommand\":{\"reelWatchEndpoint\":{\"videoId\":\"SSSSSSSSSS1\"}}}}},"
    "{\"shortsLockupViewModel\":{\"accessibilityText\":\"Caf\\u00e9 \\u2013 play Short\","
    "\"onTap\":{\"innertubeCommand\":{\"reelWatchEndpoint\":{\"videoId\":\"SSSSSSSSSS2\"}}}}},"
    "{\"lockupViewModel\":{\"contentImage\":{\"thumbnailViewModel\":{\"image\":{\"sources\":["
    "{\"url\":\"https://i.ytimg.com/vi/FIRSTVIDEO1/hqdefault.jpg?sqp=x\"}]}}},"
    "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Best of\"},"
    "\"metadata\":{\"contentMetadataViewModel\":{\"metadataRows\":[{\"metadataParts\":"
    "[{\"text\":{\"content\":\"Someone\"}}]}]}}}},"
    "\"contentId\":\"PLgoodlist-_123\",\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\"}},"
    "{\"lockupViewModel\":{\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Bad\"}}},"
    "\"contentId\":\"PL\\\"><x\",\"contentType\":\"LOCKUP_CONTENT_TYPE_PLAYLIST\"}}"
    "]};</script>";

static const char YT_PLAYLIST[] =
    "<script>var ytInitialData = {\"metadata\":{\"playlistMetadataRenderer\":{\"title\":\"Top 100\","
    "\"description\":\"This week's songs.\"}},\"contents\":["
    "{\"lockupViewModel\":{\"contentId\":\"PPPPPPPPPP1\",\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\","
    "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"Number one\"}}}}}]};</script>";

/* --- the same, after YouTube has renamed things ---------------------------------
 *
 * Every name the reader looks for changed, as YouTube changes them: the data
 * under another variable, each renderer with a new name. What is left is the
 * shape -- and the traps in it: a menu inside a video with a title of its own
 * and the video's id beside it (both ways a menu is written), a view model
 * keeping its title three levels down, a titled shelf of two, a titled list
 * whose videos have no titles, the first video again, ids that are not ids. */
static const char YT_RENAMED[] =
    "<script>var ytData = {\"contents\":{\"resultsV3\":[{\"sectionV2\":{\"items\":["
    "{\"videoRendererV9\":{\"videoId\":\"RRRRRRRRRR1\",\"title\":{\"runs\":[{\"text\":\"Renamed \\u0026 one\"}]},"
    "\"ownerText\":{\"runs\":[{\"text\":\"Owner One\",\"navigationEndpoint\":{\"browseEndpoint\":"
    "{\"browseId\":\"UCowner1\",\"canonicalBaseUrl\":\"/@one\"}}}]},"
    "\"viewCountText\":{\"simpleText\":\"10 views\"},\"lengthText\":{\"simpleText\":\"1:23\"},"
    "\"menu\":{\"menuV2\":{\"items\":[{\"item\":{\"title\":\"Add to queue\",\"command\":{\"watchEndpoint\":"
    "{\"videoId\":\"RRRRRRRRRR1\"}}}},{\"item\":{\"title\":\"Save\",\"videoId\":\"RRRRRRRRRR1\"}}]}}}},"
    "{\"lockupViewModelNext\":{\"image\":{\"thumbnailViewModel\":{}},\"metadata\":{\"lockupMetadataViewModelV2\":"
    "{\"title\":{\"content\":\"Renamed two\"},\"metadata\":{\"rows\":{\"metadataRows\":["
    "{\"metadataParts\":[{\"text\":{\"content\":\"Owner Two\"}}]},"
    "{\"metadataParts\":[{\"text\":{\"content\":\"5 views\"}},{\"text\":{\"content\":\"1 day ago\"}}]}]}},"
    "\"menuButton\":{\"sheet\":{\"listItems\":[{\"listItemViewModel\":{\"title\":{\"content\":\"Add to queue\"},"
    "\"onTap\":{\"watchEndpoint\":{\"videoId\":\"RRRRRRRRRR2\"}}}}]}}}},"
    "\"onTap\":{\"innertubeCommand\":{\"watchEndpoint\":{\"videoId\":\"RRRRRRRRRR2\"}}}}},"
    "{\"shelfV2\":{\"title\":{\"simpleText\":\"A shelf of two\"},\"items\":["
    "{\"videoRendererV9\":{\"videoId\":\"RRRRRRRRRR3\",\"title\":{\"simpleText\":\"Three\"}}},"
    "{\"videoRendererV9\":{\"videoId\":\"RRRRRRRRRR4\",\"title\":{\"simpleText\":\"Four\"}}}]}},"
    "{\"videoRendererV9\":{\"videoId\":\"RRRRRRRRRR1\",\"title\":{\"simpleText\":\"the same again\"}}},"
    "{\"playlistV3\":{\"title\":{\"simpleText\":\"Not a video\"},\"videos\":["
    "{\"watchEndpoint\":{\"videoId\":\"RRRRRRRRRR5\"}},{\"watchEndpoint\":{\"videoId\":\"RRRRRRRRRR6\"}}]}},"
    "{\"cardV2\":{\"videoId\":\"\\\"><img src=x>\",\"title\":\"bad\"}},"
    "{\"cardV2\":{\"videoId\":\"short\",\"title\":\"too short\"}},"
    "{\"cardV2\":{\"title\":\"\\\"videoId\\\":\\\"FAKEFAKEFAK\\\",\\\"title\\\":\\\"fake\\\"\"}}"
    "]}}]}};</script>";

/* A page the names still read, with one video under a new name as well: the
   shapes are only looked at when the names find too few, so a page that
   reads today reads the same. */
static const char YT_NAMED_PLUS[] =
    "<script>var ytInitialData = {\"contents\":["
    "{\"videoRenderer\":{\"videoId\":\"NNNNNNNNNN1\",\"title\":{\"simpleText\":\"n1\"}}},"
    "{\"videoRenderer\":{\"videoId\":\"NNNNNNNNNN2\",\"title\":{\"simpleText\":\"n2\"}}},"
    "{\"videoRenderer\":{\"videoId\":\"NNNNNNNNNN3\",\"title\":{\"simpleText\":\"n3\"}}},"
    "{\"videoRendererV9\":{\"videoId\":\"NNNNNNNNNN4\",\"title\":{\"simpleText\":\"n4\"}}}]};</script>";

/* A watch page whose data names nothing the reader knows: no videoDetails,
   the storyboards under another name, a live spec beside the real one, and
   the video itself among those beside it. What the video is comes from the
   tags the page has for search engines, written with references, in the
   other order, and with the video's name first where a channel's might be
   looked for. */
static const char YT_WATCH_BARE[] =
    "<html><head><meta name=\"title\" content=\"Rock &amp; roll &quot;live&quot; &lt;b&gt; &#233;&#x1F600;\">"
    "<meta content=\"Line &#39;one&#39; &lt;script&gt;\" property=\"og:description\">"
    "<meta itemprop=\"name\" content=\"Not the channel\">"
    "<meta itemprop=\"duration\" content=\"PT1H2M5S\">"
    "<span itemprop=\"author\" itemscope><link itemprop=\"url\" href=\"http://www.youtube.com/@Band\">"
    "<link itemprop=\"name\" content=\"The &amp; Band\"></span>"
    "<div itemprop=\"interactionStatistic\"><meta itemprop=\"interactionType\" "
    "content=\"https://schema.org/WatchAction\"><meta itemprop=\"userInteractionCount\" content=\"1234567\"></div>"
    "</head><body><script>var ytPlayer = {\"storyboardsV2\":{\"liveSpec\":"
    "\"https://i.ytimg.com/sb/WWWWWWWWWW1/storyboard_live_90_3x3_b1/M$M.jpg?rs=x#159#90#3#3\","
    "\"specRendererV3\":{\"spec\":\"https://i.ytimg.com/sb/WWWWWWWWWW1/storyboard3_L$L/$N.jpg?sqp=q"
    "|160#90#60#5#5#2000#M$M#rs$W\"}}};</script>"
    "<script>var ytData = {\"secondary\":["
    "{\"compactV2\":{\"videoId\":\"WWWWWWWWWW1\",\"title\":{\"simpleText\":\"Itself again\"}}},"
    "{\"compactV2\":{\"videoId\":\"MMMMMMMMMM1\",\"title\":{\"simpleText\":\"More one\"}}},"
    "{\"compactV2\":{\"videoId\":\"MMMMMMMMMM2\",\"title\":{\"simpleText\":\"More two\"}}}]};</script>"
    "</body></html>";

/* The details renamed with the fields in them as they were. */
static const char YT_WATCH_DETAILS_V2[] =
    "<script>var ytInitialPlayerResponse = {\"videoDetailsV2\":{\"videoId\":\"DDDDDDDDDD9\",\"title\":\"Kept fields\","
    "\"lengthSeconds\":\"61\",\"channelId\":\"UC9\",\"author\":\"Keeper\",\"viewCount\":\"42\",\"isLive\":false,"
    "\"shortDescription\":\"same as ever\"}};</script>";

/* A channel's feed, its entries as YouTube writes them, with an id that is
   not one and a channel id that would close its link. */
static const char YT_FEED[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><feed xmlns:yt=\"http://www.youtube.com/xml/schemas/2015\">"
    "<title>Chan</title><author><name>Chan</name></author>"
    "<entry><id>yt:video:FEEDFEEDFE1</id><yt:videoId>FEEDFEEDFE1</yt:videoId>"
    "<yt:channelId>UCBR8-60-B28hp2BmDPdntcQ</yt:channelId><title>Fish &amp; &lt;chips&gt;</title>"
    "<author><name>The &quot;Chan&quot;</name></author><published>2026-09-28T20:00:32+00:00</published>"
    "<media:group><media:title>not this</media:title><media:community><media:statistics views=\"6722\"/>"
    "</media:community></media:group></entry>"
    "<entry><yt:videoId>bad\"&gt;id</yt:videoId><title>a bad id</title></entry>"
    "<entry><yt:videoId>FEEDFEEDFE2</yt:videoId><yt:channelId>UC\"bad</yt:channelId><title>Second</title>"
    "<published>2026-09-01T00:00:00+00:00</published></entry></feed>";

/* A channel page that lists nothing and names itself only in its head. */
static const char YT_CHANNEL_EMPTY[] =
    "<html><head><meta property=\"og:title\" content=\"Quiet &amp; Co - YouTube\">"
    "<meta property=\"og:description\" content=\"We post rarely.\">"
    "<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCqqqqqqqqqqqqqqqqqqqqqq\"></head>"
    "<body><script>var ytInitialData = {\"headerV2\":{},\"tabsV2\":[]};</script></body></html>";

/* Comments whose section is no longer called comment-item-section: a token
   in a section that is not the comments, however it is titled, a panel for
   the comments with a sort menu's token in its head, and the section inside
   it with the one wanted. */
static const char YT_COMMENTS_RENAMED[] =
    "<script>ytcfg.set({\"INNERTUBE_CLIENT_VERSION\":\"2.20260925.08.00\"});</script>"
    "<script>var ytInitialData = {\"contents\":{\"results\":["
    "{\"sectionV2\":{\"contents\":[{\"continuationV2\":{\"continuationCommand\":{\"token\":\"RELATEDTOKEN1234\"}}}],"
    "\"sectionIdentifier\":\"related-items\",\"title\":\"what the comments said\"}},"
    "{\"panelV2\":{\"panelIdentifier\":\"engagement-panel-comments-v2\",\"header\":{\"sortMenu\":"
    "{\"continuationCommand\":{\"token\":\"SORTTOKEN12345678\"}}},"
    "\"content\":{\"sectionV2\":{\"contents\":[{\"continuationV2\":{\"continuationCommand\":"
    "{\"token\":\"Eg0SC2RR-_x%3Dcomments\"}}}],\"targetId\":\"comments-v2\"}}}}"
    "]}};</script>";

/* And tokens none of which is the comments': one under a title that says
   comments, a comments section with none (they are turned off), and one in
   the section beside it. */
static const char YT_COMMENTS_DECOY[] =
    "<script>var ytInitialData = {\"contents\":["
    "{\"sectionV2\":{\"title\":\"comments\",\"contents\":[{\"continuationCommand\":{\"token\":\"TITLEDECOY12345\"}}]}},"
    "{\"sectionV2\":{\"sectionIdentifier\":\"comments-v2\",\"contents\":"
    "[{\"messageV2\":{\"text\":\"Comments are turned off.\"}}]}},"
    "{\"sectionV2\":{\"sectionIdentifier\":\"related\",\"contents\":"
    "[{\"continuationCommand\":{\"token\":\"NEIGHBOURTOKEN1\"}}]}}]};</script>";

/* --- answers of the shapes Twitch's API gives ---------------------------------- */

static const char TW_STREAMS[] =
    "{\"data\":{\"streams\":{\"edges\":["
    "{\"node\":{\"title\":\"First <live>\",\"viewersCount\":12345,\"broadcaster\":{\"login\":\"first_one\","
    "\"displayName\":\"FirstOne\"},\"game\":{\"name\":\"Chess\"},"
    "\"previewImageURL\":\"https://static-cdn.jtvnw.net/previews-ttv/live_user_first_one-320x180.jpg\"}},"
    "{\"node\":{\"title\":\"Second\",\"viewersCount\":7,\"broadcaster\":{\"login\":\"second\",\"displayName\":\"\"},"
    "\"game\":null,\"previewImageURL\":\"https://evil.example/x.jpg\"}},"
    "{\"node\":{\"title\":\"nobody\",\"viewersCount\":1,\"broadcaster\":null}}"
    "]}},\"extensions\":{\"durationMilliseconds\":30}}";

static const char TW_GAMES[] =
    "{\"data\":{\"games\":{\"edges\":["
    "{\"node\":{\"name\":\"Just Chatting\",\"viewersCount\":511959,"
    "\"boxArtURL\":\"https://static-cdn.jtvnw.net/ttv-boxart/509658-144x192.jpg\"}},"
    "{\"node\":{\"name\":\"Rock & Roll\",\"viewersCount\":1000,\"boxArtURL\":\"\"}}]}}}";

static const char TW_LIVE_USER[] =
    "{\"data\":{\"user\":{\"displayName\":\"Some\\u00e9One\",\"description\":\"hi <there>\","
    "\"stream\":{\"title\":\"live now\",\"viewersCount\":42,\"game\":{\"name\":\"Chess\"},"
    "\"previewImageURL\":\"https://static-cdn.jtvnw.net/previews-ttv/live_user_x-640x360.jpg\"},"
    "\"lastBroadcast\":{\"title\":\"old\"}}}}";

static const char TW_OFF_USER[] =
    "{\"data\":{\"user\":{\"displayName\":\"Quiet\",\"description\":\"\",\"stream\":null,"
    "\"lastBroadcast\":{\"title\":\"yesterday's\"}}}}";

static const char TW_NO_USER[] = "{\"data\":{\"user\":null},\"extensions\":{}}";
static const char TW_NO_GAME[] = "{\"data\":{\"game\":null}}";

/* A past broadcast, a channel with past broadcasts, a search, and the list
   of a broadcast's frames. */
static const char TW_VIDEO_ANSWER[] =
    "{\"data\":{\"video\":{\"title\":\"Long <night>\",\"lengthSeconds\":7384,\"viewCount\":1234,"
    "\"publishedAt\":\"2026-09-27T18:24:59Z\",\"owner\":{\"login\":\"some_one\",\"displayName\":\"SomeOne\"},"
    "\"game\":{\"name\":\"Chess\"},"
    "\"previewThumbnailURL\":\"https://static-cdn.jtvnw.net/cf_vods/x/thumb/thumb0-640x360.jpg\","
    "\"seekPreviewsURL\":\"https://d2abc.cloudfront.net/abc_some_one_1/storyboards/99-info.json\"}}}";

static const char TW_VIDEO_ELSEWHERE[] =
    "{\"data\":{\"video\":{\"title\":\"x\",\"lengthSeconds\":60,\"viewCount\":1,"
    "\"seekPreviewsURL\":\"https://evil.example/storyboards/99-info.json\"}}}";

/* The larger frames listed first, so choosing the smaller is a choice. */
static const char TW_SEEK_INFO[] =
    "[{\"count\":120,\"width\":220,\"rows\":10,\"images\":[\"99-high-0.jpg\"],\"interval\":60,"
    "\"quality\":\"high\",\"cols\":5,\"height\":124},"
    "{\"count\":120,\"width\":160,\"rows\":10,\"images\":[\"99-low-0.jpg\",\"99-low-1.jpg\","
    "\"../../evil.jpg\",\"99-low-2.jpg\"],\"interval\":60,\"quality\":\"low\",\"cols\":5,\"height\":90}]";

static const char TW_CHANNEL_VIDEOS[] =
    "{\"data\":{\"user\":{\"displayName\":\"Quiet\",\"description\":\"\",\"stream\":null,"
    "\"lastBroadcast\":{\"title\":\"yesterday's\"},\"videos\":{\"edges\":["
    "{\"node\":{\"id\":\"111\",\"title\":\"First VOD\",\"lengthSeconds\":3600,\"viewCount\":42,"
    "\"publishedAt\":\"2026-09-20T10:00:00Z\","
    "\"previewThumbnailURL\":\"https://static-cdn.jtvnw.net/cf_vods/y/thumb0-320x180.jpg\",\"game\":{\"name\":\"Go\"}}},"
    "{\"node\":{\"id\":\"22\\\"x\",\"title\":\"bad id\",\"lengthSeconds\":1,\"viewCount\":1}},"
    "{\"node\":{\"id\":\"333\",\"title\":\"Processing\",\"lengthSeconds\":60,\"viewCount\":0,"
    "\"previewThumbnailURL\":\"https://vod-secure.twitch.tv/_404/404_processing_320x180.png\",\"game\":null}}"
    "]}}}}";

static const char TW_FOUND[] =
    "{\"data\":{\"searchFor\":{\"channels\":{\"items\":["
    "{\"login\":\"chess\",\"displayName\":\"Chess\",\"followers\":{\"totalCount\":1274052},\"stream\":null},"
    "{\"login\":\"blitz\",\"displayName\":\"Blitz\",\"followers\":{\"totalCount\":241133},"
    "\"stream\":{\"viewersCount\":1037,\"title\":\"games <live>\",\"game\":{\"name\":\"Chess\"},"
    "\"previewImageURL\":\"https://static-cdn.jtvnw.net/previews-ttv/live_user_blitz-320x180.jpg\"}},"
    "{\"login\":\"bad\\\"login\",\"displayName\":\"x\",\"followers\":{\"totalCount\":1},\"stream\":null}"
    "]}}}}";

/* Twitch after a change: its home page with the Client-Id its script uses,
   answers from a question with a field it no longer knows, a failure with
   nothing usable, errors beside an answer that is still whole, the smaller
   question's answers, and a channel's own page with its card tags. */
static const char TW_HOME_HTML[] =
    "<html><script>var x=1;clientId=\"bad-one\",y=cookies[\"auth-token\"]?\"OAuth \":void 0,"
    "clientId=\"kimne78kx3ncx6brgo4mv6wki5h1ko\",commonOptions={method:\"POST\"}</script></html>";

static const char TW_UNKNOWN_FIELD[] =
    "{\"errors\":[{\"message\":\"Cannot query field \\\"previewImageURL\\\" on type \\\"Stream\\\".\","
    "\"locations\":[{\"line\":1,\"column\":80}]}]}";
static const char TW_NULL_USER_ERRORS[] =
    "{\"errors\":[{\"message\":\"service timeout\",\"path\":[\"user\"]}],\"data\":{\"user\":null}}";
static const char TW_ERRORS_BUT_WHOLE[] =
    "{\"errors\":[{\"message\":\"x\",\"path\":[\"streams\",\"edges\",0,\"node\",\"game\"]}],\"data\":{\"streams\":"
    "{\"edges\":[{\"node\":{\"title\":\"a\",\"viewersCount\":1,\"broadcaster\":{\"login\":\"a\"},\"game\":null}}]}}}";
static const char TW_DATA_NULL[] = "{\"errors\":[{\"message\":\"x\"}],\"data\":null}";

static const char TW_MIN_VIDEO[] =
    "{\"data\":{\"video\":{\"title\":\"Min\",\"lengthSeconds\":61,\"owner\":{\"login\":\"a_b\",\"displayName\":\"AB\"}}}}";
static const char TW_MIN_FOUND[] =
    "{\"data\":{\"searchFor\":{\"channels\":{\"items\":[{\"login\":\"chess\",\"displayName\":\"Chess\","
    "\"stream\":null}]}}}}";

static const char TW_CHANNEL_HTML[] =
    "<html><head><meta property=\"og:site_name\" content=\"Twitch\"/>"
    "<meta content=\"Some &amp; One - Twitch\" property=\"og:title\"/>"
    "<meta property=\"og:description\" content=\"Streams &lt;chess&gt; daily &#8212; come along\"/>"
    "<meta property=\"og:image\" content=\"https://static-cdn.jtvnw.net/jtv_user_pictures/x-profile_image-300x300.png\"/>"
    "</head><body></body></html>";
static const char TW_CHANNEL_HTML_ELSEWHERE[] =
    "<html><head><meta property=\"og:title\" content=\"Else - Twitch\"/>"
    "<meta property=\"og:image\" content=\"https://evil.example/x.png\"/></head></html>";

static int tw(int kind, const char *name, const char *data) {
    return twitch_page(kind, name, data, w_len(data), 0, 0, page, (int)sizeof(page));
}

static int twq(const char *address, char *query, int cap, char *name, int ncap) {
    url_t u;
    if (!at_url(address, &u)) return -1;
    return twitch_query(&u, query, cap, name, ncap, 0);
}

/* The smaller question, asked when the whole one fails. */
static int twqm(const char *address, char *query, int cap, char *name, int ncap) {
    url_t u;
    if (!at_url(address, &u)) return -1;
    return twitch_query(&u, query, cap, name, ncap, 1);
}

static void checks(void) {
    char out[256];

    /* --- the JSON underneath --------------------------------------------------- */
    {
        const char *s = "\"a\\\"b\" rest";
        okn("a string is skipped past its end, escaped quote and all", sj_skip(s, 0, w_len(s)) == 6,
            sj_skip(s, 0, w_len(s)));
        const char *o = "{\"x\":\"}\",\"y\":[1,{\"z\":\"]\"}]} tail";
        okn("an object is skipped past its end, brackets inside strings and all",
            sj_skip(o, 0, w_len(o)) == 27, sj_skip(o, 0, w_len(o)));
        /* A key whose own name has a quote in it, written escaped, ends in
           what looks like another key; and a string ending in a backslash
           ends at a quote that is not escaped. */
        const char *k = "{\"x\\\"b\":1,\"a\":\"y\\\\\",\"b\":2}";
        int v = sj_find(k, 0, w_len(k), "b");
        ok("a key written inside another is not taken for one", v >= 0 && k[v] == '2');
        const char *e = "\"\\u00e9\\ud83d\\ude00\\n\\\\\\/\"";
        int n = sj_str(e, 0, w_len(e), out, sizeof(out));
        ok("escapes come out as UTF-8, a pair of halves as one character",
           n == 9 && (u8)out[0] == 0xC3 && (u8)out[1] == 0xA9 && (u8)out[2] == 0xF0 && (u8)out[3] == 0x9F
           && (u8)out[4] == 0x98 && (u8)out[5] == 0x80 && out[6] == '\n' && out[7] == '\\' && out[8] == '/');
        const char *r = "{\"runs\":[{\"text\":\"one \"},{\"text\":\"two\",\"bold\":true}]}";
        sj_text(r, 0, w_len(r), out, sizeof(out));
        ok("text in runs is run together", w_same(out, "one two"));
        const char *bare = "{\"n\":12345,\"t\":true}";
        sj_field(bare, 0, w_len(bare), "n", out, sizeof(out));
        int num = w_same(out, "12345");
        sj_field(bare, 0, w_len(bare), "t", out, sizeof(out));
        ok("and a number or a word comes out as it is written", num && w_same(out, "true"));

        site_page p = { out, 0, (int)sizeof(out) };
        sp_num(&p, 0); sp_raw(&p, " "); sp_num(&p, 999); sp_raw(&p, " ");
        sp_num(&p, 1000); sp_raw(&p, " "); sp_num(&p, 1234567);
        ok("counts are written with commas", w_same(out, "0 999 1,000 1,234,567"));
    }

    /* --- a search ---------------------------------------------------------------- */
    {
        int n = yt("https://www.youtube.com/results?search_query=cats+%26+dogs", YT_SEARCH);
        okn("a YouTube search is read into a page", n > 0, n);
        okn("with each video once, and none with an id that is not one", count(page, "class=\"row\"") == 2,
            count(page, "class=\"row\""));
        ok("linked to its own watch page, with its picture",
           has(page, "<a href=\"/watch?v=AAAAAAAAAA1\">")
           && has(page, "src=\"https://i.ytimg.com/vi/AAAAAAAAAA1/mqdefault.jpg\""));
        ok("its title is the runs of its title, escaped for the page",
           has(page, "<b>Cats &quot;and&quot; dogs &amp; more</b>"));
        ok("and who made it, linked to their channel, how many watched, when, and how long",
           has(page, "<a href=\"/@animals\">Animal Channel</a><br>1,234 views &middot; 2 days ago &middot; 3:21"));
        ok("and what the search found in it", has(page, "A line about cats"));
        ok("a byline in the other place it is kept is found", has(page, "Second</a><br>12 views"));
        ok("and a channel address that would close its link is not used, the channel's id is",
           has(page, "<a href=\"/channel/UC2second\">Second</a>"));
        ok("a character past the first sixty five thousand arrives whole",
           has(page, "Smile \xF0\x9F\x98\x80 &lt;script&gt;"));
        ok("and markup in a title is shown, never obeyed", !has(page, "<script") && !has(page, "<img src=x"));
        ok("a key written inside a string is not a video", !has(page, "FAKEFAKEFAK"));
        ok("the words searched for are in the title, the heading and the box",
           has(page, "<title>cats &amp; dogs - YouTube</title>") && has(page, "<h2>cats &amp; dogs</h2>")
           && has(page, "value=\"cats &amp; dogs\""));
        ok("and the box searches YouTube again", has(page, "<form action=\"/results\" method=\"get\"")
           && has(page, "name=\"search_query\""));
        ok("and the page says what was done", has(page, "Read by zelr from the data in YouTube's page"));
    }

    /* --- a video ----------------------------------------------------------------- */
    {
        int n = yt("https://www.youtube.com/watch?v=CCCCCCCCCC3", YT_WATCH);
        okn("a watch page is read into a page", n > 0, n);
        ok("headed by its title, escaped", has(page, "<h1>A &lt;b&gt;bold&lt;/b&gt; title</h1>")
           && has(page, "<title>A &lt;b&gt;bold&lt;/b&gt; title - YouTube</title>"));
        ok("with its large picture", has(page, "https://i.ytimg.com/vi/CCCCCCCCCC3/hqdefault.jpg"));
        ok("who made it, how many watched and how long it is",
           has(page, "<b><a href=\"/channel/UC3\">Maker</a></b> &middot; 1,234,567 views &middot; 1:02:05"));
        ok("its description, lines kept and escapes undone",
           has(page, "line one<br>line two \xE2\x86\x92 https://example.com/?a=1&amp;b=2"));
        ok("and that it cannot be played here, said", has(page, "no video decoder"));
        ok("the videos beside it, from the view model YouTube uses now",
           has(page, "/watch?v=DDDDDDDDDD4") && has(page, "Related &quot;one&quot;")
           && has(page, "Other Maker<br>5K views &middot; 1 year ago"));
        ok("and from the renderer it used before", has(page, "/watch?v=EEEEEEEEEE5")
           && has(page, "Old Maker<br>9 views"));
        ok("and a playlist among them, linked to its own page",
           has(page, "<a href=\"/playlist?list=PLxxxxxxxxxxxxxxxx\"><b>A playlist</b>"));
        okn("so three beside it", count(page, "class=\"row\"") == 3, count(page, "class=\"row\""));

        ok("frames from the video, from its storyboards",
           has(page, "<h2>frames from the video</h2>") && has(page, "One every 2 seconds"));
        ok("from the level whose frames are the largest no wider than 160",
           has(page, "https://i.ytimg.com/sb/CCCCCCCCCC3/storyboard3_L2/M0.jpg?sqp=abc&amp;sigh=rs$CCC")
           && !has(page, "storyboard3_L1") && !has(page, "storyboard3_L3"));
        ok("the first, middle and last sheets, each with the stretch of the video it covers",
           has(page, "L2/M1.jpg") && has(page, "L2/M2.jpg") && count(page, "/sb/") == 3
           && has(page, "0:00 to 0:48") && has(page, "0:50 to 1:38") && has(page, "1:40 to 1:58"));

        n = yt("https://www.youtube.com/watch?v=HHHHHHHHHH8", YT_ELSEWHERE);
        ok("but no frames from anywhere but YouTube's picture server",
           n > 0 && !has(page, "evil.example") && !has(page, "frames from the video"));

        n = yt("https://www.youtube.com/watch?v=GGGGGGGGGG7", YT_LIVE);
        ok("a stream that is on now says live rather than a length",
           n > 0 && has(page, "<b>Station</b> &middot; 10 views &middot; live") && !has(page, "0:00"));
        ok("and has no frames, since a stream still going has no storyboard to stand for it",
           !has(page, "frames from the video"));
        ok("and has nothing beside it to list, said", has(page, "nothing else was listed"));
    }

    /* --- a channel, and the front page --------------------------------------------- */
    {
        int n = yt("https://www.youtube.com/@someone", YT_CHANNEL);
        ok("a channel is read into a page, named and described",
           n > 0 && has(page, "<title>Some &amp; One - YouTube</title>")
           && has(page, "<h1>Some &amp; One</h1>") && has(page, "<p>About us.</p>"));
        ok("with its videos", has(page, "/watch?v=FFFFFFFFFF6") && has(page, "<b>Upload</b>"));
        n = yt("https://www.youtube.com/results?search_query=tunes", YT_MIXED);
        ok("a Short in a search is a row, linked to its ordinary watch page",
           n > 0 && has(page, "<a href=\"/watch?v=SSSSSSSSSS1\"><b>Tiny &lt;tune&gt;, 1.3 million views</b>")
           && has(page, "SSSSSSSSSS1/mqdefault.jpg") && has(page, "Short"));
        ok("and a title ending in an accented letter keeps it when the words after it go",
           has(page, "<b>Caf\xC3\xA9</b>"));
        ok("a playlist in a search is linked to its own page, with its first video's picture",
           has(page, "<a href=\"/playlist?list=PLgoodlist-_123\"><b>Best of</b>")
           && has(page, "FIRSTVIDEO1/mqdefault.jpg"));
        ok("but not one whose list would close its link", !has(page, "Bad") && !has(page, "<x"));
        n = yt("https://www.youtube.com/playlist?list=PLabc", YT_PLAYLIST);
        ok("a playlist's own page is headed by its name, with its videos",
           n > 0 && has(page, "<title>Top 100 - YouTube</title>") && has(page, "<h1>Top 100</h1>")
           && has(page, "This week's songs.") && has(page, "/watch?v=PPPPPPPPPP1"));

        {
            static char tok[512], ver[48];
            int got = yt_comments_token(YT_WITH_COMMENTS, w_len(YT_WITH_COMMENTS), tok, sizeof(tok),
                                        ver, sizeof(ver));
            ok("a watch page's comments are asked for with the token of its comments section",
               got && w_same(tok, "Eg0SC2RR-_x%3D") && w_same(ver, "2.20260925.08.00"));
            got = yt_comments_token(YT_NO_COMMENTS, w_len(YT_NO_COMMENTS), tok, sizeof(tok),
                                    ver, sizeof(ver));
            ok("and not at all when the section has none", !got && !tok[0]);
            got = yt_comments_token(YT_BAD_TOKEN, w_len(YT_BAD_TOKEN), tok, sizeof(tok), ver, sizeof(ver));
            ok("nor with a token that would close the quotes it is sent in", !got && !tok[0]);
            site_page cp = { page, 0, (int)sizeof(page) };
            page[0] = 0;
            int c = yt_comments_write(&cp, YT_COMMENTS_ANSWER, w_len(YT_COMMENTS_ANSWER), 20);
            okn("the answer's comments are written, one each, and none for a comment with no words",
                c == 2 && count(page, "class=\"comment\"") == 2, c);
            ok("who, when, how liked, how many replies, and what they said, escaped",
               has(page, "<b>@YouTube</b> <small>1 year ago &middot; 321K likes &middot; 963 replies"
                         "</small><br>never &lt;gave&gt; us up<br>second line"));
            ok("with one like said as one, and no replies not said at all",
               has(page, "<b>@b</b> <small>2 days ago &middot; 1 like</small><br>one"));
            static char doc[256];
            w_copy(doc, sizeof(doc), "<html><body><p>x</p></body></html>\n", sizeof(doc));
            int dn = site_append(doc, w_len(doc), (int)sizeof(doc), "<h2>c</h2>", 10);
            ok("and they go at the end of the page, inside it",
               dn == w_len(doc) && w_same(doc, "<html><body><p>x</p><h2>c</h2></body></html>\n"));
        }

        n = yt("https://www.youtube.com/", YT_FRONT);
        ok("the front page, which lists nothing to a stranger, says so and offers the search",
           n > 0 && has(page, "did not list any videos") && has(page, "search_query"));
        n = yt("https://www.example.com/results?search_query=x", YT_SEARCH);
        okn("the same data on another site is not read", n == 0, n);
        n = yt("https://www.youtube.com/about", "<html><body>about us</body></html>");
        okn("nor a YouTube page with no data in it", n == 0, n);
    }

    /* --- Twitch: the questions ------------------------------------------------------ */
    {
        static char q[1024], name[256];
        int k = twq("https://www.twitch.tv/", q, sizeof(q), name, sizeof(name));
        ok("Twitch's front page asks who is live", k == TW_LIVE && has(q, "streams(first:24)"));
        k = twq("https://www.twitch.tv/directory/", q, sizeof(q), name, sizeof(name));
        ok("its directory asks for the categories", k == TW_CATEGORIES && has(q, "games(first:30)"));
        k = twq("https://www.twitch.tv/directory/category/Just%20Chatting", q, sizeof(q), name, sizeof(name));
        ok("a category asks for that category's streams, by its name",
           k == TW_CATEGORY && w_same(name, "Just Chatting") && has(q, "game(name:\"Just Chatting\")"));
        k = twq("https://www.twitch.tv/directory/game/x%22)%7Buser", q, sizeof(q), name, sizeof(name));
        okn("a category's name cannot close the quotes it is written into",
            k == TW_CATEGORY && count(q, "\"") == 2 && w_same(name, "x){user"), count(q, "\""));
        k = twq("https://twitch.tv/some_one/videos", q, sizeof(q), name, sizeof(name));
        ok("a channel, from any page of it, asks about that channel",
           k == TW_CHANNEL && w_same(name, "some_one") && has(q, "user(login:\"some_one\")"));
        k = twq("https://www.twitch.tv/a%22b", q, sizeof(q), name, sizeof(name));
        ok("and a login is only what logins are made of", k == TW_CHANNEL && w_same(name, "a"));
        k = twq("https://www.twitch.tv/%22", q, sizeof(q), name, sizeof(name));
        okn("so one made of nothing else asks nothing", k == TW_NONE, k);
        k = twq("https://www.example.com/", q, sizeof(q), name, sizeof(name));
        okn("and another site is not Twitch", k == TW_NONE, k);
        k = twq("https://www.twitch.tv/videos/2885710155", q, sizeof(q), name, sizeof(name));
        ok("a past broadcast asks for that broadcast and its frames",
           k == TW_VIDEO && has(q, "video(id:\"2885710155\")") && has(q, "seekPreviewsURL"));
        k = twq("https://www.twitch.tv/videos/12%22x", q, sizeof(q), name, sizeof(name));
        okn("and only by a number made of what numbers are made of", k == TW_NONE, k);
        k = twq("https://www.twitch.tv/search?term=chess+%22club%22", q, sizeof(q), name, sizeof(name));
        ok("a search asks for channels by the words, with nothing that could close them",
           k == TW_SEARCH && has(q, "searchFor(userQuery:\"chess club\"") && count(q, "\"") == 4);
        k = twq("https://www.twitch.tv/search", q, sizeof(q), name, sizeof(name));
        ok("and with no words asks nothing", k == TW_SEARCH && !q[0]);
        k = twq("https://www.twitch.tv/some_one", q, sizeof(q), name, sizeof(name));
        ok("a channel asks for its past broadcasts too", k == TW_CHANNEL && has(q, "videos(first:10)"));

        static char body[2048];
        twitch_body("query{game(name:\"a\\b\")}", body, sizeof(body));
        ok("the question is sent as JSON, its quotes escaped",
           w_same(body, "{\"query\":\"query{game(name:\\\"a\\\\b\\\")}\"}"));
    }

    /* --- Twitch: the pages -------------------------------------------------------- */
    {
        int n = tw(TW_LIVE, "", TW_STREAMS);
        okn("who is live is written as a page", n > 0, n);
        okn("one row a stream, and none for a stream with nobody streaming it",
            count(page, "class=\"row\"") == 2, count(page, "class=\"row\""));
        ok("linked to the channel, with its picture and its title escaped",
           has(page, "<a href=\"/first_one\">") && has(page, "live_user_first_one-320x180.jpg")
           && has(page, "<b>First &lt;live&gt;</b>"));
        ok("who, what, and how many watching", has(page, "FirstOne &middot; Chess<br>12,345 watching"));
        ok("a stream with no category and no display name still shows", has(page, "second<br>7 watching"));
        ok("and a picture from anywhere but Twitch's own server is left out", !has(page, "evil.example"));
        ok("and the page says it cannot play them", has(page, "no video decoder"));

        n = tw(TW_CATEGORIES, "", TW_GAMES);
        ok("the categories are written as a page, each linked by its name",
           n > 0 && count(page, "class=\"row\"") == 2
           && has(page, "href=\"/directory/game/Just%20Chatting\"") && has(page, "511,959 watching"));
        ok("with anything in a name that means something in an address escaped",
           has(page, "href=\"/directory/game/Rock%20%26%20Roll\"") && has(page, "<b>Rock &amp; Roll</b>"));

        n = tw(TW_CHANNEL, "somename", TW_LIVE_USER);
        ok("a live channel shows what it is streaming, to how many",
           n > 0 && has(page, "<h1>Some\xC3\xA9One</h1>") && has(page, "<b>live:</b> live now")
           && has(page, "Chess &middot; 42 watching") && has(page, "live_user_x-640x360.jpg"));
        ok("and what it says about itself, escaped", has(page, "<p>hi &lt;there&gt;</p>"));
        n = tw(TW_CHANNEL, "quiet", TW_OFF_USER);
        ok("a channel that is not live says so, and what it last streamed",
           n > 0 && has(page, "offline; last streamed: yesterday's") && !has(page, "watching"));
        n = tw(TW_CHANNEL, "nobody_here", TW_NO_USER);
        ok("a name Twitch has never heard of says that",
           n > 0 && has(page, "<h1>nobody_here</h1>") && has(page, "knows nobody by that name"));
        ok("every Twitch page has a search box that searches Twitch",
           has(page, "<form action=\"/search\" method=\"get\">") && has(page, "name=\"term\""));

        n = tw(TW_CHANNEL, "quiet", TW_CHANNEL_VIDEOS);
        ok("a channel lists its past broadcasts, linked to their own pages",
           n > 0 && has(page, "<h2>past broadcasts</h2>") && has(page, "<a href=\"/videos/111\">")
           && has(page, "Go &middot; 42 views &middot; 1:00:00 &middot; 2026-09-20"));
        ok("but not one whose number would close its link, nor a picture from elsewhere",
           !has(page, "bad id") && has(page, "/videos/333") && !has(page, "vod-secure"));

        n = twitch_page(TW_VIDEO, "99", TW_VIDEO_ANSWER, w_len(TW_VIDEO_ANSWER),
                        TW_SEEK_INFO, w_len(TW_SEEK_INFO), page, (int)sizeof(page));
        ok("a past broadcast has a page of its own",
           n > 0 && has(page, "<h1>Long &lt;night&gt;</h1>") && has(page, "thumb0-640x360.jpg")
           && has(page, "<b><a href=\"/some_one\">SomeOne</a></b> &middot; Chess &middot; 1,234 views"
                        " &middot; 2:03:04 &middot; 2026-09-27"));
        ok("with frames from it, from the smaller of Twitch's storyboards",
           has(page, "<h2>frames from the broadcast</h2>") && has(page, "One every 60 seconds")
           && has(page, "https://d2abc.cloudfront.net/abc_some_one_1/storyboards/99-low-0.jpg")
           && !has(page, "99-high-0.jpg"));
        ok("the first, middle and last sheets, each with the stretch it covers",
           has(page, "99-low-1.jpg") && has(page, "99-low-2.jpg") && count(page, "storyboards/") == 3
           && has(page, "0:00 to 49:00") && has(page, "50:00 to 1:39:00") && has(page, "1:40:00 to 1:59:00"));
        ok("and never a sheet whose name climbs out of the list's own place", !has(page, "evil"));
        n = twitch_page(TW_VIDEO, "98", TW_VIDEO_ELSEWHERE, w_len(TW_VIDEO_ELSEWHERE),
                        TW_SEEK_INFO, w_len(TW_SEEK_INFO), page, (int)sizeof(page));
        ok("nor frames listed anywhere but Twitch's own video servers",
           n > 0 && !has(page, "frames from the broadcast") && !has(page, "evil.example"));

        n = tw(TW_SEARCH, "chess", TW_FOUND);
        ok("a search lists the channels it found, live ones with what they are streaming",
           n > 0 && has(page, "<a href=\"/blitz\"><b>Blitz</b></a><br><small>live: games &lt;live&gt;"
                            " &middot; Chess<br>1,037 watching &middot; 241,133 followers")
           && has(page, "live_user_blitz-320x180.jpg"));
        ok("and the others as offline", has(page, "<a href=\"/chess\"><b>Chess</b></a> <small>offline"
                                                  " &middot; 1,274,052 followers"));
        ok("but none whose login would close its link", !has(page, "bad"));

        n = tw(TW_CATEGORY, "Nothing", TW_NO_GAME);
        ok("and a category with nobody in it says that", n > 0 && has(page, "nobody is live here just now"));
    }

    /* --- YouTube, after it has renamed things ------------------------------------ */
    {
        int n = yt("https://www.youtube.com/results?search_query=renamed", YT_RENAMED);
        okn("data under none of the names it had is still read, for the video ids in it", n > 0, n);
        okn("with every renderer renamed, the videos are found by their shape", count(page, "class=\"row\"") == 4,
            count(page, "class=\"row\""));
        ok("each under its own title and linked by its id, in the order listed",
           has(page, "<a href=\"/watch?v=RRRRRRRRRR1\"><b>Renamed &amp; one</b>")
           && has(page, "<a href=\"/watch?v=RRRRRRRRRR2\"><b>Renamed two</b>")
           && has(page, "<a href=\"/watch?v=RRRRRRRRRR3\"><b>Three</b>")
           && has(page, "<a href=\"/watch?v=RRRRRRRRRR4\"><b>Four</b>")
           && site_search(page, 0, n, "RRRRRRRRRR1") < site_search(page, 0, n, "RRRRRRRRRR2")
           && site_search(page, 0, n, "RRRRRRRRRR2") < site_search(page, 0, n, "RRRRRRRRRR3"));
        ok("with who made it and what the site says of it, from where it keeps them",
           has(page, "<a href=\"/@one\">Owner One</a><br>10 views &middot; 1:23")
           && has(page, "Owner Two<br>5 views &middot; 1 day ago"));
        ok("a menu's title inside a video is never the video's", !has(page, "Add to queue") && !has(page, "Save"));
        ok("nor a list's, and a video in a list with no title of its own is left out",
           !has(page, "A shelf of two") && !has(page, "Not a video") && !has(page, "RRRRRRRRRR5"));
        ok("each once, and nothing for an id that is not one or a key inside a string",
           count(page, "RRRRRRRRRR1\"><b>") == 1 && !has(page, "the same again") && !has(page, "<img src=x")
           && !has(page, "too short") && !has(page, "FAKEFAKEFAK"));
        n = yt("https://www.youtube.com/results?search_query=n", YT_NAMED_PLUS);
        okn("and a page the names still read is read as it was, shapes not looked at",
            n > 0 && count(page, "class=\"row\"") == 3 && !has(page, "n4"), count(page, "class=\"row\""));

        n = yt("https://www.youtube.com/watch?v=WWWWWWWWWW1", YT_WATCH_BARE);
        ok("a watch page whose data no longer says what the video is is read from its tags",
           n > 0 && has(page, "<h1>Rock &amp; roll &quot;live&quot; &lt;b&gt; \xC3\xA9\xF0\x9F\x98\x80</h1>")
           && has(page, "<title>Rock &amp; roll &quot;live&quot; &lt;b&gt; \xC3\xA9\xF0\x9F\x98\x80 - YouTube</title>"));
        ok("who made it from inside its author, linked, not the first name on the page, how many and how long",
           has(page, "<b><a href=\"/@Band\">The &amp; Band</a></b> &middot; 1,234,567 views &middot; 1:02:05")
           && !has(page, "Not the channel"));
        ok("and its description, references undone and markup escaped", has(page, "<p>Line 'one' &lt;script&gt;</p>"));
        ok("with its picture from its id, and how it was read, said",
           has(page, "https://i.ytimg.com/vi/WWWWWWWWWW1/hqdefault.jpg")
           && has(page, "tags YouTube's page has for search engines"));
        ok("frames from a storyboard spec found without its renderer's name, not from the live one",
           has(page, "https://i.ytimg.com/sb/WWWWWWWWWW1/storyboard3_L0/M0.jpg?sqp=q&amp;sigh=rs$W")
           && count(page, "/sb/") == 3 && !has(page, "storyboard_live"));
        ok("the videos beside it found by their shape, and never itself among them",
           has(page, "<a href=\"/watch?v=MMMMMMMMMM1\"><b>More one</b>") && has(page, "More two")
           && !has(page, "Itself again") && !has(page, "/watch?v=WWWWWWWWWW1"));
        n = yt("https://www.youtube.com/watch?v=DDDDDDDDDD9", YT_WATCH_DETAILS_V2);
        ok("details under a new name are found by the video's id beside its length",
           n > 0 && has(page, "<h1>Kept fields</h1>")
           && has(page, "<b><a href=\"/channel/UC9\">Keeper</a></b> &middot; 42 views &middot; 1:01")
           && has(page, "<p>same as ever</p>") && !has(page, "search engines"));

        static char spec[256];
        int sl = yt_storyboard(YT_WATCH_BARE, w_len(YT_WATCH_BARE), spec, sizeof(spec));
        ok("the spec found is the one with levels in it",
           sl > 0 && w_starts_fold(spec, "https://i.ytimg.com/sb/WWWWWWWWWW1/storyboard3_L$L/"));

        site_page fp = { page, 0, (int)sizeof(page) };
        page[0] = 0;
        int rows = yt_feed_write(&fp, YT_FEED, w_len(YT_FEED), 24);
        okn("a channel's feed is read into rows, one an upload", rows == 2 && count(page, "class=\"row\"") == 2, rows);
        ok("each linked by its id and titled, with who, how many watched and the day, escaped",
           has(page, "<a href=\"/watch?v=FEEDFEEDFE1\"><b>Fish &amp; &lt;chips&gt;</b>")
           && has(page, "FEEDFEEDFE1/mqdefault.jpg")
           && has(page, "<a href=\"/channel/UCBR8-60-B28hp2BmDPdntcQ\">The &quot;Chan&quot;</a><br>6,722 views"
                        " &middot; 2026-09-28")
           && !has(page, "not this"));
        ok("but none for an id that is not one, nor a channel link that would close itself",
           !has(page, "a bad id") && has(page, "<b>Second</b></a><br><small><br>2026-09-01</small>")
           && !has(page, "UC\"bad"));
        ok("under a heading that says where they came from",
           has(page, "<h2>latest uploads</h2>") && has(page, "From the channel's feed"));
        fp.n = 0;
        page[0] = 0;
        rows = yt_feed_write(&fp, "<feed></feed>", 13, 24);
        ok("and a feed with nothing in it adds nothing, heading and all", rows == 0 && fp.n == 0 && !page[0]);

        static char cid[32];
        const char *m1 = "{\"externalId\":\"UCBR8-60-B28hp2BmDPdntcQ\"}";
        int c1 = yt_channel_id(m1, w_len(m1), cid, sizeof(cid))
                 && w_same(cid, "UCBR8-60-B28hp2BmDPdntcQ");
        const char *m2 = "<meta itemprop=\"identifier\" content=\"UCuAXFkgsw1L7xaCfnd5JJOw\">";
        int c2 = yt_channel_id(m2, w_len(m2), cid, sizeof(cid)) && w_same(cid, "UCuAXFkgsw1L7xaCfnd5JJOw");
        const char *m3 = "<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCaaaaaaaaaaaaaaaaaaaaaa\">";
        int c3 = yt_channel_id(m3, w_len(m3), cid, sizeof(cid)) && w_same(cid, "UCaaaaaaaaaaaaaaaaaaaaaa");
        ok("a channel's id is read from its data, its search engine tag or its own address", c1 && c2 && c3);
        const char *j1 = "{\"externalId\":\"UC\\\"><script>aaaaaaaaaaaa\"}";
        const char *j2 = "<meta itemprop=\"identifier\" content=\"dQw4w9WgXcQ\">";
        const char *j3 = "<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCshort\">";
        int bad = yt_channel_id(j1, w_len(j1), cid, sizeof(cid)) + yt_channel_id(j2, w_len(j2), cid, sizeof(cid))
                + yt_channel_id(j3, w_len(j3), cid, sizeof(cid));
        okn("but never anything that is not one", bad == 0 && !cid[0], bad);
        n = yt("https://www.youtube.com/@quiet", YT_CHANNEL_EMPTY);
        ok("a channel page that lists nothing asks for the channel's feed, and says why",
           n > 0 && w_same(site_youtube_feed_for, "UCqqqqqqqqqqqqqqqqqqqqqq")
           && has(page, "did not list this channel's videos"));
        ok("and is named by its card when its data does not name it",
           has(page, "<h1>Quiet &amp; Co</h1>") && has(page, "<p>We post rarely.</p>")
           && has(page, "<title>Quiet &amp; Co - YouTube</title>"));
        n = yt("https://www.youtube.com/@someone", YT_CHANNEL);
        int listed = n > 0 && !site_youtube_feed_for[0];
        n = yt("https://www.youtube.com/results?search_query=quiet", YT_CHANNEL_EMPTY);
        ok("while one that lists its videos does not, nor a page that is not a channel's",
           listed && n > 0 && !site_youtube_feed_for[0] && !has(page, "this channel's videos"));

        static char tok[512], ver[48];
        int got = yt_comments_token(YT_COMMENTS_RENAMED, w_len(YT_COMMENTS_RENAMED), tok, sizeof(tok),
                                    ver, sizeof(ver));
        ok("a comments token is found without the section's name, in the smallest object saying comments",
           got && w_same(tok, "Eg0SC2RR-_x%3Dcomments") && w_same(ver, "2.20260925.08.00"));
        got = yt_comments_token(YT_COMMENTS_DECOY, w_len(YT_COMMENTS_DECOY), tok, sizeof(tok), ver, sizeof(ver));
        ok("but none from a title that says comments, or from beside comments that have none",
           !got && !tok[0]);
    }

    /* --- Twitch, after it has changed things ---------------------------------------- */
    {
        static char id[64];
        int l = twitch_client_id(TW_HOME_HTML, w_len(TW_HOME_HTML), id, sizeof(id));
        ok("the Client-Id is read from Twitch's own page when the one known is refused",
           l == 30 && w_same(id, "kimne78kx3ncx6brgo4mv6wki5h1ko"));
        const char *junk[] = { "clientId=\"short\"", "clientId=\"has-dash-aaaaaaaaaaaaaaaaaaaaaaaa\"",
                               "clientId=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"",
                               "clientId=\"abc\\\"><script>aaaaaaaaaaaaaaaaaaaaaaa\"",
                               "clientId=\"aaaaaaaaaaaaaaaaaaaaaaaa", 0 };
        int taken = 0;
        for (int k = 0; junk[k]; k++) taken += twitch_client_id(junk[k], w_len(junk[k]), id, sizeof(id)) > 0;
        okn("but only letters and digits, as long as one is", taken == 0 && !id[0], taken);
        ok("and asked for only when the client was refused, not the question",
           twitch_refused(400, "", 0) && twitch_refused(401, "", 0)
           && twitch_refused(500, "The \"Client-ID\" header is invalid.", 34)
           && !twitch_refused(200, TW_STREAMS, w_len(TW_STREAMS)) && !twitch_refused(500, "busy", 4));

        static char q[1024], name[256];
        int k1 = twqm("https://www.twitch.tv/", q, sizeof(q), name, sizeof(name));
        int m1 = k1 == TW_LIVE && has(q, "streams(first:24){edges{node{title viewersCount broadcaster{login "
                                         "displayName}}}}") && !has(q, "previewImageURL") && !has(q, "game{");
        int k2 = twqm("https://www.twitch.tv/some_one", q, sizeof(q), name, sizeof(name));
        int m2 = k2 == TW_CHANNEL && has(q, "user(login:\"some_one\"){displayName stream{title viewersCount}}")
              && !has(q, "videos(");
        int k3 = twqm("https://www.twitch.tv/videos/123", q, sizeof(q), name, sizeof(name));
        int m3 = k3 == TW_VIDEO && has(q, "video(id:\"123\"){title lengthSeconds owner{login displayName}}");
        int k4 = twqm("https://www.twitch.tv/directory", q, sizeof(q), name, sizeof(name));
        int m4 = k4 == TW_CATEGORIES && has(q, "games(first:30){edges{node{name viewersCount}}}")
              && !has(q, "boxArtURL");
        int k5 = twqm("https://www.twitch.tv/directory/category/Chess", q, sizeof(q), name, sizeof(name));
        int m5 = k5 == TW_CATEGORY && has(q, "game(name:\"Chess\"){streams(first:24)");
        int k6 = twqm("https://www.twitch.tv/search?term=a%22b", q, sizeof(q), name, sizeof(name));
        int m6 = k6 == TW_SEARCH && has(q, "searchFor(userQuery:\"ab\"") && !has(q, "followers")
              && count(q, "\"") == 4;
        ok("the smaller question asks each page for only who, what and how many",
           m1 && m2 && m3 && m4 && m5 && m6);

        int f1 = twitch_failed(TW_LIVE, TW_UNKNOWN_FIELD, w_len(TW_UNKNOWN_FIELD));
        int f2 = twitch_failed(TW_CHANNEL, TW_NULL_USER_ERRORS, w_len(TW_NULL_USER_ERRORS));
        int f3 = twitch_failed(TW_LIVE, TW_DATA_NULL, w_len(TW_DATA_NULL));
        ok("an answer is taken for a failed question when it has errors and nothing to make the page of",
           f1 && f2 && f3);
        int g1 = twitch_failed(TW_LIVE, TW_STREAMS, w_len(TW_STREAMS));
        int g2 = twitch_failed(TW_CHANNEL, TW_NO_USER, w_len(TW_NO_USER));
        int g3 = twitch_failed(TW_LIVE, TW_ERRORS_BUT_WHOLE, w_len(TW_ERRORS_BUT_WHOLE));
        int g4 = twitch_failed(TW_VIDEO, TW_VIDEO_ANSWER, w_len(TW_VIDEO_ANSWER));
        ok("but not an answer, nor nobody by that name, nor errors beside what the page needs",
           !g1 && !g2 && !g3 && !g4);

        int n = tw(TW_VIDEO, "7", TW_MIN_VIDEO);
        ok("the smaller answer still makes the page, with no count it did not give written as none",
           n > 0 && has(page, "<b><a href=\"/a_b\">AB</a></b> &middot; 1:01") && !has(page, "views"));
        n = tw(TW_SEARCH, "chess", TW_MIN_FOUND);
        ok("so a channel found by a search is offline, and not followed by nobody",
           n > 0 && has(page, "<b>Chess</b></a> <small>offline</small>") && !has(page, "followers"));

        n = twitch_page_meta("some_one", TW_CHANNEL_HTML, w_len(TW_CHANNEL_HTML), page, (int)sizeof(page));
        ok("a channel the API says nothing of is read from its own page's card",
           n > 0 && has(page, "<title>Some &amp; One - Twitch</title>") && has(page, "<h1>Some &amp; One</h1>")
           && has(page, "<p>Streams &lt;chess&gt; daily \xE2\x80\x94 come along</p>")
           && has(page, "src=\"https://static-cdn.jtvnw.net/jtv_user_pictures/x-profile_image-300x300.png\""));
        ok("with the way to the others, and saying how it was read and that nothing plays",
           has(page, "<form action=\"/search\" method=\"get\">") && has(page, "because Twitch's API did not answer")
           && has(page, "no video decoder"));
        n = twitch_page_meta("else", TW_CHANNEL_HTML_ELSEWHERE, w_len(TW_CHANNEL_HTML_ELSEWHERE), page,
                             (int)sizeof(page));
        ok("but no picture from anywhere but Twitch's own server",
           n > 0 && has(page, "<h1>Else</h1>") && !has(page, "evil.example") && !has(page, "<p></p>"));
        n = twitch_page_meta("x", "<html><head></head></html>", 26, page, (int)sizeof(page));
        okn("and a page whose head says nothing makes no page", n == 0, n);
    }

    /* --- the foot of the window ---------------------------------------------------
     *
     * These pages have long titles and the browser says long things about
     * them, and the status bar drew the two halves through each other. */
    {
        const char *long_status = "read from the data in YouTube's page: it cannot play the videos, "
                                  "24 pictures, encrypted";
        const char *long_title = "Rick Astley - Never Gonna Give You Up (Official Video) (4K Remaster) - YouTube";
        const char *l = long_status, *r = long_title;
        ui_status_fit(600, 1, &l, &r);
        int lw = face_w(l, UI_FACE_BODY), rw = face_w(r, UI_FACE_BODY);
        okn("a long status and a long title fit beside each other in the bar",
            lw + rw <= 600 - 44, lw + rw);
        int n = w_len(r);
        ok("the title giving way, cut where a character ends and saying it was cut",
           n > 3 && r[n - 1] == '.' && r[n - 2] == '.' && r[n - 3] == '.'
           && r[0] == 'R' && lw >= rw);
        l = long_status;
        r = long_title;
        ui_status_fit(600, 0, &l, &r);
        ok("and in the classic look, each inside its own panel",
           face_w(r, UI_FACE_BODY) <= UI_STATUS_RIGHT_W && face_w(l, UI_FACE_BODY) <= 600 - 154);
        l = "3 links on this page";
        r = "a page";
        ui_status_fit(860, 1, &l, &r);
        ok("while short ones are left alone", w_same(l, "3 links on this page") && w_same(r, "a page"));
        /* Cut at every width from 20 to 120, so wherever the cut falls it
           falls somewhere next to a character of two bytes. */
        static char fit[64];
        int halves = 0;
        for (int wide = 20; wide <= 120; wide++) {
            ui_fit_text("caf\xC3\xA9\xC3\xA9\xC3\xA9 caf\xC3\xA9\xC3\xA9\xC3\xA9 caf\xC3\xA9\xC3\xA9\xC3\xA9",
                        wide, UI_FACE_BODY, fit, (int)sizeof(fit));
            int m = w_len(fit);
            if (m < 4 || (u8)fit[m - 4] >= 0xC0) halves++;
        }
        okn("and a cut never leaves half a character", halves == 0, halves);
    }

    /* --- Google ------------------------------------------------------------------ */
    {
        url_t u;
        static char q[256];
        at_url("https://www.google.com/search?q=cats+%26+dogs&hl=en", &u);
        int n = site_google_search(&u, q, sizeof(q));
        ok("a Google search gives up its words", n > 0 && w_same(q, "cats & dogs"));
        at_url("https://www.google.co.uk/search?hl=en&q=x", &u);
        ok("from a country's Google too", site_google_search(&u, q, sizeof(q)) && w_same(q, "x"));
        at_url("https://google.com.au/search?q=a", &u);
        ok("and a country written in two parts", site_google_search(&u, q, sizeof(q)) && w_same(q, "a"));
        at_url("https://google.example.org/search?q=a", &u);
        int no1 = site_google_search(&u, q, sizeof(q));
        at_url("https://notgoogle.com/search?q=a", &u);
        int no2 = site_google_search(&u, q, sizeof(q));
        ok("but not from a site with Google in its name", !no1 && !no2);
        at_url("https://www.google.com/", &u);
        int no3 = site_google_search(&u, q, sizeof(q));
        at_url("https://www.google.com/searchbyimage?q=a", &u);
        int no4 = site_google_search(&u, q, sizeof(q));
        ok("nor from a Google page that is not its search", !no3 && !no4);
    }
}

/* One real page, read, and what was found in it said on the console, for
   tools/sitecheck.py to hold against the real sites. */
static int live(const char *address) {
    url_t u;
    if (!url_parse(address, &u)) { puts("SITETEST_LIVE not an address\n"); return 1; }
    int n = 0;
    if (site_is_twitch(&u)) {
        n = site_twitch(&u, page, (int)sizeof(page));
    } else if (site_is_youtube(&u)) {
        int cap = 4 * 1024 * 1024;
        char *buf = (char *)malloc((u64)cap);
        if (!buf) { puts("SITETEST_LIVE no memory\n"); return 1; }
        response_t r;
        int rc = web_get(&u, buf, cap, &r);
        puts("SITETEST_LIVE status ");
        putn(rc);
        puts(" bytes ");
        putn(rc >= 0 ? r.len : 0);
        putc('\n');
        n = rc == 200 ? site_youtube(&u, r.body, r.len, page, (int)sizeof(page)) : rc;
        static char extra[64 * 1024];
        if (n > 0 && w_starts_fold(u.path, "/watch")) {
            int cn = site_youtube_comments(r.body, r.len, extra, (int)sizeof(extra));
            if (cn > 0) n = site_append(page, n, (int)sizeof(page), extra, cn);
        } else if (n > 0 && site_youtube_feed_for[0]) {
            int fn = site_youtube_feed(site_youtube_feed_for, extra, (int)sizeof(extra));
            puts("SITETEST_LIVE feed ");
            putn(fn);
            putc('\n');
            if (fn > 0) n = site_append(page, n, (int)sizeof(page), extra, fn);
        }
        free(buf);
    }
    puts("SITETEST_LIVE page ");
    putn(n);
    puts(" rows ");
    putn(n > 0 ? count(page, "class=\"row\"") : 0);
    puts(" pictures ");
    putn(n > 0 ? count(page, "<img ") : 0);
    puts(" frames ");
    putn(n > 0 ? count(page, "/sb/") + count(page, "/storyboards/") : 0);
    puts(" comments ");
    putn(n > 0 ? count(page, "class=\"comment\"") : 0);
    putc('\n');
    /* The first few titles, so a run can be read by a person as well. */
    int shown = 0, len = n > 0 ? n : 0;
    for (int at = site_search(page, 0, len, "<b>"); at >= 0 && shown < 3;
         at = site_search(page, at + 3, len, "<b>")) {
        int e = site_search(page, at, len, "</b>");
        if (e < 0) break;
        puts("SITETEST_LIVE title ");
        for (int i = at + 3; i < e && i < at + 83; i++) putc(page[i]);
        putc('\n');
        shown++;
    }
    return n > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 2 && w_same(argv[1], "live")) exit(live(argv[2]));

    puts("sites read another way\n");
    checks();
    if (failed) {
        puts("SITETEST_FAIL ");
        putn(failed);
        putc('\n');
        exit(1);
    }
    puts("SITETEST_PASS\n");
    exit(0);
}
