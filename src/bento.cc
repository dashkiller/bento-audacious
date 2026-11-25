/*
 * bento.cc - "Bento" main player panel for Audacious 4.x (Qt build)
 *
 * Milestone 1: draws the top-left player panel of the Winamp 5 "Bento" skin
 * (time display, song ticker, volume, seek bar, transport buttons, shuffle,
 * repeat, mute, lightning bolt) using the skin's own PNG files (now embedded
 * via bento_assets.h), and wires it to Audacious playback.
 *
 * Milestone 2: adds a playlist pane on the right matching the Bento / Winamp
 * look (numbered entries, orange text, durations, current-track highlight,
 * double-click to play, scrollbar, total time footer).
 *
 * Milestone 3: window is resizable; a centre video panel (Nullsoft Video
 * placeholder, tab strip, bottom chrome) sits between the player and the
 * playlist — layout matches the classic Bento main window.
 *
 * All coordinates were taken from Bento's xml/player-normal-group.xml and
 * xml/player-elements.xml.  Skin assets (player.png, numfont.png) are
 * embedded in bento_assets.h — no external Bento skin folder is required.
 * Set BENTO_ZOOM=2 (or 3, 4) to scale the panel up on high-resolution screens.
 *
 * Milestone 5: the playlist behaves like Audacious's own: click / Ctrl / Shift
 * selection (stored in the real Audacious playlist), keyboard navigation,
 * drag-to-reorder, file drag-and-drop (onto the list or onto a playlist tab),
 * cut / copy / paste, queue, song info and a right-click menu.
 *
 * Milestone 6: playlist tabs get Audacious-style right-click menu (Play, Rename,
 * Remove), double-click to rename, a "+" button for a new playlist, and
 * horizontal scrolling when there are more tabs than fit.
 *
 * Milestone 4: full IfacePlugin (Qt).  Select "Bento Interface" as the
 * Audacious interface; it replaces the stock Qt/GTK UI and runs as the
 * sole main window (QApplication::exec is owned by this plugin).
 */
#include "bento_assets.h"

#include <atomic>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMimeData>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QWidget>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/interface.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/probe.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include <libaudqt/iface.h>
#include <libaudqt/libaudqt.h>

/* Audacious's own build passes this as a compiler flag; out-of-tree builds
 * don't get it from pkg-config, so define it here if it is missing. */
#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

namespace {

    /* ---- geometry (Bento "player.layout" group, in skin pixels) ------------ */

    constexpr int GW = 226;        // width of the player group
    constexpr int PLAYER_W = 232;  // group + room for the lightning bolt
    constexpr int PLAYER_H = 99;

    constexpr int PL_MIN_W = 120;  // playlist minimum width
    constexpr int PL_DEF_W = 430;  // playlist default width
    constexpr int PL_ROW = 15;     // row height
    constexpr int PL_TAB_H = 22;   // playlist-selector tabs at top of pane
    constexpr int PL_HEADER = 2;   // padding below tabs before first row
    constexpr int PL_BOTTOM = 2;   // padding between the last visible row and the footer
    constexpr int PL_FOOTER = 24;  // bottom bar
    constexpr int PL_PAD_X = 8;    // left/right text padding
    constexpr int SCROLL_W = 12;   // scrollbar width

    constexpr int ART_BOX = 92;    // album-art frame (Bento info.component.holder)
    constexpr int ART_INSET = 4;   // cover sits at 4,4 inside the frame
    constexpr int ART_GAP = 4;     // gap between the player group and the frame
    constexpr int ART_MARGIN = 3;  // space right of the frame
    constexpr int ART_COL_W = ART_GAP + ART_BOX + ART_MARGIN;
    constexpr int ART_Y = 0;       // top-aligned with the player skin (same top border)
    constexpr int ART_IMG = ART_BOX - 2 * ART_INSET;

    constexpr int LEFT_MIN_W = PLAYER_W + ART_COL_W; // splitter stops at player + art
    constexpr int TAB_H = 22;      // tab strip height under player
    constexpr int VID_FOOTER = 24; // video bottom chrome
    constexpr int TOP_PAD = 4;     // gap under the OS title bar
    constexpr int PL_TAB_X0 = 0;   // first playlist tab's inset from the pane's left edge
    constexpr int TITLEBAR_H = 0;  // we don't draw a fake title bar yet

    constexpr int MIN_W = LEFT_MIN_W + PL_MIN_W;
    constexpr int MIN_H = PLAYER_H + TAB_H + 120;
    constexpr int DEF_W = 800;
    constexpr int DEF_H = 560;

    const QRect kTimeRect(16, 5, 99, 27);
    const QRect kTickerBox(73, 3, 150, 20);
    const QRect kVolSlider(GW - 81, 43, 80, 10);
    const QRect kSeekSlider(3, 59, GW - 4, 10);

    constexpr int kVolThumbW = 17;
    constexpr int kSeekThumbW = 31;

    enum BtnId
    {
        B_PREV,
        B_PLAY,
        B_PAUSE,
        B_STOP,
        B_NEXT,
        B_EJECT,
        B_SHUFFLE,
        B_REPEAT,
        B_MUTE,
        B_BOLT,
        B_COUNT
    };

    const QRect kBtnRect[B_COUNT] = {
        QRect(0, 70, 26, 24),        // prev
        QRect(25, 70, 22, 24),       // play
        QRect(46, 70, 22, 24),       // pause
        QRect(67, 70, 22, 24),       // stop
        QRect(88, 70, 26, 24),       // next
        QRect(GW - 112, 73, 25, 17), // eject
        QRect(GW - 83, 73, 32, 17),  // shuffle
        QRect(GW - 54, 73, 27, 17),  // repeat
        QRect(GW - 111, 40, 25, 14), // mute
        QRect(187, 62, 42, 37),      // bolt
    };

    /* ---- colours ------------------------------------------------------------ */

    const QRgb kTimeColor = qRgb(221, 210, 104);
    const QRgb kSliderLight = qRgb(241, 178, 85);
    const QColor kInfoBright(251, 203, 100);
    const QColor kInfoDim(130, 106, 54);
    const QColor kStatusColor(125, 104, 54);

    const QColor kPlBg(12, 12, 12);
    const QColor kPlText(255, 207, 103);       // entry text + lengths; brighter than Winamp's 227,184,91 to offset thinner antialiased glyphs
    const QColor kPlCurrentText(225, 225, 225); // text of the playing entry (Winamp's is 200,200,200; a bit brighter here because Qt's text renders thinner)
    const QColor kPlCurrent(255, 200, 60);     // queue "[n]" marker
    const QColor kPlDim(140, 110, 50);
    const QColor kPlFooterBg(32, 36, 40);
    const QColor kPlScrollBg(28, 30, 32);
    const QColor kPlScrollThumb(70, 72, 75);
    const QColor kPlDivider(40, 42, 45);
    const QColor kPlSelected(22, 22, 18);   // selected rows (same tint as the old click/hover highlight)
    const QColor kPlDrop(255, 200, 60);     // drop-position marker

    const QColor kChromeBg(42, 46, 50);
    const QColor kChromeDark(28, 30, 32);
    const QColor kChromeEdge(60, 64, 68);
    const QColor kTabActive(0x3A, 0x3D, 0x40); // selected tab fill, both tab strips
    const QColor kTabInactive(38, 40, 44);
    const QColor kTabText(180, 180, 180);
    const QColor kVidBg(0, 0, 0);
    const QColor kVidIcon(220, 220, 220);

    /* ---- tiny 4x5 pixel font ------------------------------------------------ */

    struct PixGlyph
    {
        char ch;
        unsigned char rows[5];
    };

    const PixGlyph kGlyphs[] = {
        {'0', {0x6, 0x9, 0x9, 0x9, 0x6}}, {'1', {0x2, 0x6, 0x2, 0x2, 0x7}},
        {'2', {0x6, 0x9, 0x2, 0x4, 0xF}}, {'3', {0xE, 0x1, 0x6, 0x1, 0xE}},
        {'4', {0x9, 0x9, 0xF, 0x1, 0x1}}, {'5', {0xF, 0x8, 0xE, 0x1, 0xE}},
        {'6', {0x6, 0x8, 0xE, 0x9, 0x6}}, {'7', {0xF, 0x1, 0x2, 0x4, 0x4}},
        {'8', {0x6, 0x9, 0x6, 0x9, 0x6}}, {'9', {0x6, 0x9, 0x7, 0x1, 0x6}},
        {'K', {0x9, 0xA, 0xC, 0xA, 0x9}}, {'B', {0xE, 0x9, 0xE, 0x9, 0xE}},
        {'P', {0xE, 0x9, 0xE, 0x8, 0x8}}, {'S', {0x7, 0x8, 0x6, 0x1, 0xE}},
        {'H', {0x9, 0x9, 0xF, 0x9, 0x9}}, {'Z', {0xF, 0x1, 0x6, 0x8, 0xF}},
        {'M', {0x9, 0xF, 0xF, 0x9, 0x9}}, {'O', {0x6, 0x9, 0x9, 0x9, 0x6}},
        {'N', {0x9, 0xD, 0xB, 0x9, 0x9}}, {'T', {0xE, 0x4, 0x4, 0x4, 0x4}},
        {'E', {0xF, 0x8, 0xE, 0x8, 0xF}}, {'R', {0xE, 0x9, 0xE, 0xA, 0x9}},
    };

    void drawPixText(QPainter & p, int x, int y, const char * text,
                     const QColor & color)
    {
        for (; *text; text++, x += 5)
        {
            for (const PixGlyph & g : kGlyphs)
            {
                if (g.ch != *text)
                    continue;
                for (int r = 0; r < 5; r++)
                    for (int c = 0; c < 4; c++)
                        if ((g.rows[r] >> (3 - c)) & 1)
                            p.fillRect(x + c, y + r, 1, 1, color);
                break;
            }
        }
    }

    /* ---- helpers ------------------------------------------------------------ */

    std::atomic<bool> g_meta_dirty{true};
    std::atomic<bool> g_art_dirty{true};

    void meta_hook(void *, void *)
    {
        g_meta_dirty = true;
    }

    void art_hook(void *, void *)
    {
        g_art_dirty = true;
    }

    QString qs(const String & s)
    {
        const char * c = s;
        return c ? QString::fromUtf8(c) : QString();
    }

    /* "Artist - Title" for one playlist entry; shared by the ticker and the rows */
    QString entryDisplayTitle(const Playlist & pl, int i, const Tuple & t)
    {
        QString song = qs(t.get_str(Tuple::Title));
        if (song.isEmpty())
            song = qs(t.get_str(Tuple::FormattedTitle));
        if (song.isEmpty())
        {
            QString fn = qs(pl.entry_filename(i));
            int slash = fn.lastIndexOf('/');
            song = (slash >= 0) ? fn.mid(slash + 1) : fn;
        }
        QString artist = qs(t.get_str(Tuple::Artist));
        if (!artist.isEmpty() && !song.isEmpty())
            return artist + " - " + song;
        return song;
    }

    QLinearGradient playerGradient()
    {
        QLinearGradient bg(0, 0, 0, PLAYER_H);
        // Subtle lift through the middle, matching the supplied gradient reference.
        bg.setColorAt(0.00, QColor(0x2E, 0x33, 0x3B));
        bg.setColorAt(0.20, QColor(0x31, 0x37, 0x3E));
        bg.setColorAt(0.40, QColor(0x34, 0x3B, 0x40));
        bg.setColorAt(0.55, QColor(0x35, 0x3C, 0x40));
        bg.setColorAt(0.70, QColor(0x34, 0x3A, 0x3E));
        bg.setColorAt(0.85, QColor(0x30, 0x36, 0x39));
        bg.setColorAt(1.00, QColor(0x2C, 0x32, 0x35));
        return bg;
    }

    QPixmap tintSprite(const QImage & src, QRgb color, double alphaGain)
    {
        QImage img = src.convertToFormat(QImage::Format_ARGB32);
        int maxLum = 1;
        for (int y = 0; y < img.height(); y++)
        {
            const QRgb * line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x++)
                if (qAlpha(line[x]) > 0)
                    maxLum = qMax(maxLum, (qRed(line[x]) + qGreen(line[x]) + qBlue(line[x])) / 3);
        }
        for (int y = 0; y < img.height(); y++)
        {
            QRgb * line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); x++)
            {
                QRgb px = line[x];
                double lum = ((qRed(px) + qGreen(px) + qBlue(px)) / 3.0) / maxLum;
                if (lum > 1.0)
                    lum = 1.0;
                int a = qMin(255, int(qAlpha(px) * alphaGain));
                line[x] = qRgba(int(qRed(color) * lum), int(qGreen(color) * lum),
                                int(qBlue(color) * lum), a);
            }
        }
        return QPixmap::fromImage(img);
    }

    /* Recolours a sprite the way Bento's "PlayerButtonActive" colour group does: a
     * pixel's brightness (mean of R, G, B) is multiplied by a gain per channel and
     * clamped; alpha is untouched.  That group's definition lives in the "Big Bento"
     * colour presets (not in the skin zip), so the gains were fitted against a real
     * Winamp/Bento screenshot of the orange theme. */
    QPixmap tintGain(const QImage & src, double kr, double kg, double kb)
    {
        QImage img = src.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); y++)
        {
            QRgb * line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); x++)
            {
                QRgb px = line[x];
                const double lum = (qRed(px) + qGreen(px) + qBlue(px)) / 3.0;
                line[x] = qRgba(qMin(255, int(lum * kr + 0.5)), qMin(255, int(lum * kg + 0.5)),
                                qMin(255, int(lum * kb + 0.5)), qAlpha(px));
            }
        }
        return QPixmap::fromImage(img);
    }

    QString formatLength(int ms)
    {
        if (ms <= 0)
            return QString();
        int s = ms / 1000;
        return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    }

    /* ---- Audacious API shims -------------------------------------------------
     * Calls into the less common parts of the Playlist / audqt API are funnelled
     * through these few functions.  If your headers spell one differently, the
     * fix is one line here instead of a hunt through the widget code. */

    void shimSongInfo(Playlist pl, int row)
    {
        audqt::infowin_show(pl, row);
    }

    /* Opens Audacious's own "Import Playlist" file dialog.  It loads into the
     * *active* playlist. */
    void shimImportPlaylist()
    {
        audqt::fileopener_show(audqt::FileMode::ImportPlaylist);
    }

    /* Opens Audacious's own "Export Playlist" file dialog (the one in its Playlist
     * menu).  It exports the *active* playlist. */
    void shimExportPlaylist()
    {
        audqt::fileopener_show(audqt::FileMode::ExportPlaylist);
    }

    /* Moves the selected entries; `anchor` is a selected row, `distance` is how
     * many rows it should travel (negative = up). */
    void shimShift(Playlist pl, int anchor, int distance)
    {
        pl.shift_entries(anchor, distance);
    }

    /* Position of `row` in the play queue, or -1 if it is not queued. */
    int shimQueuePos(Playlist pl, int row)
    {
        return pl.queue_find_entry(row);
    }

    void shimQueueToggle(Playlist pl, int row)
    {
        int q = pl.queue_find_entry(row);
        if (q >= 0)
            pl.queue_remove(q, 1);
        else
            pl.queue_insert(-1, row);
    }

    Playlist shimNewPlaylist()
    {
        return Playlist::new_playlist();
    }

    void shimSetTitle(Playlist pl, const QString & title)
    {
        pl.set_title(title.toUtf8().constData());
    }

    void shimRemovePlaylist(Playlist pl)
    {
        pl.remove_playlist();
    }

    /* Moves playlist `from` so that it ends up at index `to`. */
    void shimReorderPlaylist(int from, int to)
    {
        Playlist::reorder_playlists(from, to, 1);
    }

    /* Cover art for `file`, decoded and scaled to `size`.  Returns a null image
     * if there is none; sets `queued` if the load is still running (an
     * "art ready" hook follows). */
    QImage shimLoadArt(const QString & file, int size, bool & queued)
    {
        bool q = false;
        QImage img;
        {
            AudArtPtr art = aud_art_request(file.toUtf8().constData(), AUD_ART_DATA, &q);
            const Index<char> * data = art.data();
            if (data && data->len() > 0)
                img = QImage::fromData(reinterpret_cast<const uchar *>(data->begin()),
                                       int(data->len()));
        } // release the AudArtPtr: it pins the art cache entry
        queued = q;
        if (img.isNull())
            return img;
        return img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    PlaylistAddItem shimAddItem(const QByteArray & uri)
    {
        return PlaylistAddItem{String(uri.constData())};
    }

    void insertUrls(Playlist pl, int at, const QList<QUrl> & urls)
    {
        Index<PlaylistAddItem> items;
        for (const QUrl & u : urls)
            items.append(shimAddItem(u.toEncoded()));
        if (items.len() > 0)
            pl.insert_items(at, std::move(items), false);
    }

    QPoint dropPoint(QDropEvent * e)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return e->position().toPoint() - QPoint(0, TOP_PAD);
#else
        return e->pos() - QPoint(0, TOP_PAD);
#endif
    }

    /* ---- the widget --------------------------------------------------------- */

    class BentoWidget : public QWidget
    {
    public:
        BentoWidget();

    protected:
        void paintEvent(QPaintEvent *) override;
        void resizeEvent(QResizeEvent * e) override;
        void closeEvent(QCloseEvent * e) override;
        void mousePressEvent(QMouseEvent * e) override;
        void mouseMoveEvent(QMouseEvent * e) override;
        void mouseReleaseEvent(QMouseEvent * e) override;
        void mouseDoubleClickEvent(QMouseEvent * e) override;
        void wheelEvent(QWheelEvent * e) override;
        void leaveEvent(QEvent *) override;
        void keyPressEvent(QKeyEvent * e) override;
        void contextMenuEvent(QContextMenuEvent * e) override;
        void dragEnterEvent(QDragEnterEvent * e) override;
        void dragMoveEvent(QDragMoveEvent * e) override;
        void dragLeaveEvent(QDragLeaveEvent * e) override;
        void dropEvent(QDropEvent * e) override;

    private:
        enum class Drag { None, Volume, Seek, Scroll, Splitter, PlReorder, TabReorder, Rubber };

        void tick();
        void refreshMeta();
        void refreshArt(bool filenameChanged);
        int tabSlotAt(int x) const;
        void cancelTabDrag();
        void refreshPlaylist();
        void activate(int id);
        void openFiles();
        void toggleMute();
        void setVolumeFromX(int x);
        void noteVolumeChange();
        void seekByWheel(int dy); // middle button held + wheel: +/-5 s per notch
        void setSeekFromX(int x);
        void playEntry(int index);
        void recomputeLayout();

        QPoint logical(QMouseEvent * e) const;
        QPoint toSkin(const QPoint & pt) const;
        int buttonAt(const QPoint & pt) const;
        void buttonSprite(int id, int state, int & sx, int & sy) const;

        void drawGrid(QPainter & p, const QPixmap & src, int sx, int sy, int lw,
                      int mw, int rw, int h, int x, int y, int w);
        void drawTime(QPainter & p);
        QString timeString() const; // text shown in the LCD time display
        int timeEndX() const;       // right edge of that display, in skin coords
        void drawTicker(QPainter & p);
        void drawSongInfo(QPainter & p, int dx);
        void drawStatus(QPainter & p, int dx);
        void drawPlayer(QPainter & p);
        void drawArt(QPainter & p);
        void drawTabs(QPainter & p);
        void drawVideo(QPainter & p);
        void drawNullsoftIcon(QPainter & p, int cx, int cy);
        void drawPlaylist(QPainter & p);

        int leftColumnWidth() const; // everything left of the playlist
        int playerWidth() const;     // stretched player band width (left column minus art)
        QRect artRect() const;       // album-art frame, pinned to the column's right edge
        int playlistRowsVisible() const;
        int entryAtY(int y) const;
        QRect playlistArea() const;
        int listTop() const; // content y of the first playlist row
        int viewportHeight() const;  // height of the area the rows are drawn in
        bool pinnedToBottom() const; // scrolled to the end with a partial row above
        int firstRow() const;        // index of the first (possibly partly hidden) row drawn
        int firstRowOffset() const;  // y offset of that row relative to listTop()
        QRect scrollbarRect() const;
        QRect thumbTrackRect() const; // the part of the track the thumb moves in
        QRect thumbRect() const;
        QRect videoArea() const;
        QRect tabArea() const;
        QRect splitterRect() const;
        QRect playerArea() const;
        QRect btnRect(int id) const;
        QRect volSliderRect() const;
        QRect seekSliderRect() const;
        QRect tickerRect() const;
        int maxPlaylistWidth() const;
        QRect plTabBarRect() const;
        QRect plTabRect(int index) const;
        int plTabAt(const QPoint & pt) const;
        void activatePlaylist(int index);
        void refreshPlaylistTabs();
        void ensureCurrentVisible();
        QRect prefsButtonRect() const; // wrench in video footer

        // playlist selection / editing
        int selectedCount() const;
        void selectOnly(int row);
        void selectNone();
        void selectAll();
        void selectRange(int a, int b, bool additive);
        void toggleSelect(int row);
        void moveFocus(int delta, bool extend);
        void scrollToRow(int row);
        int dropRowAt(int y) const; // insertion boundary 0..n for a y position
        void moveSelected(int anchorRow, int dropRow);
        void removeSelected();
        void copySelected(bool cut);
        void pasteAtFocus();
        void queueToggleSelected();
        void songInfoFocus();
        bool autoScroll(); // scrolls while dragging near the list edges
        int contentYAt(int y) const;             // widget y -> y in list content (scrolls with the list)
        void updateRubber(const QPoint & pt);    // drag-select band: update rows it touches

        // playlist tabs
        QRect plusButtonRect() const;
        int tabsTotalWidth() const;
        int tabsAvailWidth() const;
        void ensureTabVisible(int index);
        void newPlaylist();
        void renamePlaylist(int tab);
        void removePlaylist(int tab);
        void showTabMenu(int tab, const QPoint & globalPos);

        // skin
        bool m_ok = false;
        QString m_error;
        QPixmap m_sheet;
        QPixmap m_light;
        QPixmap m_glow; // orange-tinted "active" halos for mute / shuffle / repeat (132x31)
        QPixmap m_digits[12];
        int m_zoom = 1;
        QFont m_tickerFont;
        QFont m_plFont;
        QFont m_footerFont; // playlist footer: fixed size, not affected by Ctrl+wheel
        int m_plFontPx = 11; // playlist entry font size, Ctrl+wheel over the pane
        int plRow() const { return qMax(PL_ROW, m_plFontPx + 3); }
        QFont m_tabFont;

        // layout (logical / unscaled pixels)
        int m_w = DEF_W;
        int m_h = DEF_H;
        int m_plW = PL_DEF_W; // current playlist width
        int m_leftW = DEF_W - PL_DEF_W; // left column width the splitter last set; window resizes keep it

        // playback state
        bool m_playing = false;
        bool m_paused = false;
        int m_time = 0;
        int m_seekTarget = -1; // ms; shown instead of the core's time until a seek lands
        qint64 m_seekHoldUntil = 0; // ms on m_clock
        int m_length = 0;
        int m_volume = 0;
        bool m_volumeKnown = false;
        QElapsedTimer m_clock;
        qint64 m_volReadoutUntil = 0; // ms on m_clock
        bool m_volReadoutShown = false;
        int m_premute = -1;
        bool m_shuffle = false;
        bool m_repeat = false;
        int m_bitrate = 0, m_samplerate = 0, m_channels = 0;
        QString m_ticker;
        int m_scroll = 0;
        int m_scrollDiv = 0;
        int m_seekWheelAccum = 0; // accumulates wheel delta for middle-button seeking
        bool m_remaining = false;

        // playlist
        struct PlEntry
        {
            QString title;
            QString length;
            int length_ms = 0;
            bool selected = false;
        };
        QVector<PlEntry> m_entries;
        int m_position = -1;
        int m_lastPlayPl = -1;  // playing song's playlist/entry seen last tick
        int m_lastPlayPos = -1;
        int m_plScroll = 0;
        int m_plHover = -1;
        int m_totalLength = 0;
        int m_focus = -1;       // keyboard focus row
        int m_anchor = -1;      // start of a Shift-range
        int m_focusPl = -1;     // playlist the focus/anchor belong to
        int m_pressRow = -1;    // row under the left button, -1 if none
        QPoint m_pressPos;
        bool m_pressCollapse = false; // plain click on an already selected row
        int m_dropRow = -1;     // insertion marker (row boundary), -1 = hidden
        int m_rubberX0 = 0, m_rubberX1 = 0;   // drag-select band, x in widget coords
        int m_rubberY0 = 0, m_rubberY1 = 0;   // y in list content coords (so it follows scrolling)
        int m_rubberLo = -1, m_rubberHi = -1; // rows currently selected by the band
        bool m_extDrag = false; // a file drag from outside is hovering
        bool m_plusHover = false;
        int m_lastTabActive = -1;
        int m_lastTabCount = -1;

        // playlist selector tabs (one per open Audacious playlist)
        QStringList m_plTitles;
        int m_activePl = 0;   // index of active playlist
        int m_plTabHover = -1;
        int m_plTabScroll = 0; // horizontal offset if many tabs
        int m_pressTab = -1;    // tab under the left button, -1 if none
        int m_tabDropSlot = -1; // tab-reorder insertion marker (boundary 0..n)

        // album art
        QString m_artFile;
        QPixmap m_art; // already scaled to ART_IMG

        // tabs (display only for now)
        int m_activeTab = 1; // 0=Media Library 1=Video 2=Visualization
        static constexpr const char * kTabNames[] = {
            "Media Library", "Video", "Visualization"};
            static constexpr int kTabCount = 3;

            // interaction
            int m_hover = -1;
            int m_down = -1;
            Drag m_drag = Drag::None;
            double m_dragFrac = 0.0;
            int m_scrollDragY = 0;
            int m_scrollDragStart = 0;
            int m_splitDragX = 0;
            int m_splitDragStartW = 0;

            QTimer m_timer;
    };

    BentoWidget::BentoWidget()
    {
        setWindowTitle("Winamp");
        m_clock.start();
        setMouseTracking(true);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(true);
        setMinimumSize(MIN_W, MIN_H + TOP_PAD);
        resize(DEF_W, DEF_H);

        bool zoomOk = false;
        int z = qgetenv("BENTO_ZOOM").toInt(&zoomOk);
        m_zoom = zoomOk ? qBound(1, z, 4) : 1;
        // zoom scales the *player skin* drawing only; overall window is free-size

        // Load skin assets from embedded data in bento_assets.h
        if (!m_sheet.loadFromData(player_png, player_png_len))
        {
            m_error = "Failed to load embedded Bento player skin.";
            AUDERR("bento: could not load embedded player.png data\n");
        }
        else
        {
            QPixmap numfont;
            if (numfont.loadFromData(numfont_png, numfont_png_len))
            {
                static const int cells[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15};
                for (int i = 0; i < 12; i++)
                    m_digits[i] = tintSprite(
                        numfont.copy(cells[i] * 15, 27, 15, 27).toImage(), kTimeColor, 2.0);
            }
            m_light = tintSprite(m_sheet.copy(175, 61, 16, 2).toImage(), kSliderLight, 1.0);
            // the sheet stores the active-state halos in blue-grey; recolour to the orange theme
            m_glow = tintGain(m_sheet.copy(0, 295, 132, 31).toImage(), 1.325, 1.083, 0.498);
            m_ok = true;
        }

        m_plFont.setFamilies({"Arial", "Liberation Sans", "Arimo", "Helvetica",
            "DejaVu Sans"});
        m_plFont.setPixelSize(m_plFontPx);

        // footer text uses the same family but never changes size
        m_footerFont = m_plFont;
        m_footerFont.setPixelSize(11);

        // Same family as playlist entries
        m_tickerFont = m_plFont;
        m_tickerFont.setPixelSize(11);
        m_tickerFont.setBold(true);

        m_tabFont.setFamilies({"Arial", "Liberation Sans", "Arimo", "Helvetica",
            "DejaVu Sans"});
        m_tabFont.setPixelSize(11);

        recomputeLayout();

        // Do not call tick() here — QApplication / Audacious state may still be
        // settling when the interface is switched at runtime.  The timer will
        // fire after the event loop is running.
        connect(&m_timer, &QTimer::timeout, this, [this]() { tick(); });
        m_timer.setInterval(33);
        m_timer.start();
    }

    void BentoWidget::recomputeLayout()
    {
        m_w = width();
        m_h = height() - TOP_PAD;
        // the playlist absorbs width changes first; the left column only gives way
        // once the playlist is at its minimum, and regains its width before the
        // playlist grows again
        m_plW = qBound(PL_MIN_W, m_w - m_leftW, maxPlaylistWidth());
    }

    void BentoWidget::resizeEvent(QResizeEvent * e)
    {
        QWidget::resizeEvent(e);
        recomputeLayout();
        // keep scroll in range after height change
        int vis = playlistRowsVisible();
        int n = m_entries.size();
        if (m_plScroll > qMax(0, n - vis))
            m_plScroll = qMax(0, n - vis);
        update();
    }

    /* ---- state polling ------------------------------------------------------ */

    void BentoWidget::refreshMeta()
    {
        // Build classic Winamp-style ticker: "Artist - Title (m:ss)"
        QString title;
        int len = 0;

        m_bitrate = m_samplerate = m_channels = 0;

        if (aud_drct_get_playing() && aud_drct_get_ready())
        {
            aud_drct_get_info(m_bitrate, m_samplerate, m_channels);
            len = aud_drct_get_length();
        }

        // title and art follow the playing song, whichever playlist tab is
        // selected; with nothing playing fall back to the selected playlist's
        // current entry
        Playlist pl = Playlist::playing_playlist();
        if (!pl.exists())
            pl = Playlist::active_playlist();
        int pos = pl.get_position();
        if (pos >= 0)
        {
            Tuple t = pl.entry_tuple(pos, Playlist::NoWait);
            title = entryDisplayTitle(pl, pos, t);
            if (title.isEmpty())
                title = qs(aud_drct_get_title());
            if (len <= 0)
                len = t.get_int(Tuple::Length);
        }
        else
        {
            title = qs(aud_drct_get_title());
        }

        if (title.isEmpty())
            title = "Audacious";

        if (len > 0)
        {
            int s = len / 1000;
            title += QString(" (%1:%2)").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
        }

        if (title != m_ticker)
        {
            m_ticker = title;
            m_scroll = 0;
        }

        const QString file = (pos >= 0) ? qs(pl.entry_filename(pos)) : QString();
        const bool fileChanged = (file != m_artFile);
        m_artFile = file;
        if (g_art_dirty.exchange(false) || fileChanged)
            refreshArt(fileChanged);
    }

    void BentoWidget::refreshArt(bool filenameChanged)
    {
        if (m_artFile.isEmpty())
        {
            m_art = QPixmap();
            return;
        }

        bool queued = false;
        QImage img = shimLoadArt(m_artFile, ART_IMG, queued);
        if (!img.isNull())
            m_art = QPixmap::fromImage(img);
        else if (filenameChanged || !queued)
            m_art = QPixmap(); // keep the old cover only while a load is pending
    }

    void BentoWidget::refreshPlaylistTabs()
    {
        const int n = Playlist::n_playlists();
        m_plTitles.clear();
        m_plTitles.reserve(n);
        m_activePl = 0;

        Playlist active = Playlist::active_playlist();
        for (int i = 0; i < n; i++)
        {
            Playlist pl = Playlist::by_index(i);
            QString title = qs(pl.get_title());
            if (title.isEmpty())
                title = QString("Playlist %1").arg(i + 1);
            m_plTitles.append(title);
            if (pl == active)
                m_activePl = i;
        }

        // playlist count changed under a tab drag: abandon it, no move
        if (m_pressTab >= m_plTitles.size())
            cancelTabDrag();

        // scroll the tab strip to the active tab when it (or the tab count) changes
        if (m_activePl != m_lastTabActive || n != m_lastTabCount)
        {
            m_lastTabActive = m_activePl;
            m_lastTabCount = n;
            ensureTabVisible(m_activePl);
        }
        m_plTabScroll = qBound(0, m_plTabScroll, qMax(0, tabsTotalWidth() - tabsAvailWidth()));
    }

    void BentoWidget::activatePlaylist(int index)
    {
        if (index < 0 || index >= Playlist::n_playlists())
            return;
        Playlist::by_index(index).activate();
        m_plScroll = 0;
        refreshPlaylistTabs();
        refreshPlaylist();
        update();
    }

    void BentoWidget::refreshPlaylist()
    {
        refreshPlaylistTabs();

        Playlist pl = Playlist::active_playlist();
        int n = pl.n_entries();
        m_entries.resize(n);
        m_totalLength = 0;
        m_position = pl.get_position();

        for (int i = 0; i < n; i++)
        {
            Tuple t = pl.entry_tuple(i, Playlist::NoWait);
            QString title = entryDisplayTitle(pl, i, t);
            int len = t.get_int(Tuple::Length);
            m_entries[i].title = title;
            m_entries[i].length_ms = len;
            m_entries[i].selected = pl.entry_selected(i);
            m_entries[i].length = formatLength(len);
            if (len > 0)
                m_totalLength += len;
        }

        // focus / anchor belong to one playlist; reset when it changes
        const int plIdx = pl.index();
        int vis = playlistRowsVisible();
        if (plIdx != m_focusPl)
        {
            m_focusPl = plIdx;
            m_focus = m_anchor = -1;

            // switched playlists: centre the current entry, else the focused or first selected one
            int target = m_position;
            if (target < 0 || target >= n)
                target = pl.get_focus();
            if (target < 0 || target >= n)
            {
                target = -1;
                for (int i = 0; i < n && target < 0; i++)
                    if (m_entries[i].selected)
                        target = i;
            }
            m_plScroll = target < 0 ? 0 : qBound(0, target - vis / 2, qMax(0, n - vis));
        }
        if (m_focus >= n)
            m_focus = n - 1;
        if (m_anchor >= n)
            m_anchor = n - 1;

        if (m_plScroll > qMax(0, n - vis))
            m_plScroll = qMax(0, n - vis);
    }

    void BentoWidget::ensureCurrentVisible()
    {
        int pos = m_position;
        if (pos < 0)
            return;
        int vis = playlistRowsVisible();
        if (pos < m_plScroll)
            m_plScroll = pos;
        else if (pos >= m_plScroll + vis)
            m_plScroll = pos - vis + 1;
    }

    void BentoWidget::tick()
    {
        bool changed = false;

        bool playing = aud_drct_get_playing();
        bool paused = playing && aud_drct_get_paused();
        bool ready = playing && aud_drct_get_ready();
        int time = ready ? aud_drct_get_time() : 0;
        int length = ready ? aud_drct_get_length() : 0;

        // Seeking is asynchronous: for a moment after release the core can still
        // report the pre-seek time. Hold the target until the reported time lands
        // near it (or a timeout passes) so the thumb doesn't jump back.
        if (m_seekTarget >= 0)
        {
            if (!ready || qAbs(time - m_seekTarget) < 1000 ||
                m_clock.elapsed() >= m_seekHoldUntil)
                m_seekTarget = -1;
            else
                time = m_seekTarget;
        }
        int volume = aud_drct_get_volume_main();
        bool shuffle = aud_get_bool(nullptr, "shuffle");
        bool repeat = aud_get_bool(nullptr, "repeat");

        if (m_premute >= 0 && volume > 0)
            m_premute = -1;

        const int prevPos = m_position;
        const int prevPl = m_activePl;

        if (g_meta_dirty.exchange(false) || playing != m_playing || length != m_length)
        {
            refreshMeta();
            refreshPlaylist();
            changed = true;
        }
        else if (g_art_dirty.exchange(false))
        {
            refreshArt(false);
            changed = true;
        }

        {
            Playlist pl = Playlist::active_playlist();
            int plIdx = pl.index();
            if (plIdx != m_activePl)
            {
                refreshPlaylist();
                changed = true;
            }

            // Always re-read position — refreshPlaylist may have updated m_position
            // already, so compare against the value from the start of this tick.
            int pos = pl.get_position();
            if (pos != m_position)
            {
                m_position = pos;
                changed = true;
            }
        }

        // a new song started (e.g. the previous one ended): select its entry like a
        // left-click would, in whichever playlist it is, shown or not
        if (playing)
        {
            Playlist pp = Playlist::playing_playlist();
            const int ppIdx = pp.index();
            const int ppPos = pp.get_position();
            if ((ppIdx != m_lastPlayPl || ppPos != m_lastPlayPos) && ppPos >= 0)
            {
                if (pp == Playlist::active_playlist())
                {
                    if (ppPos < m_entries.size())
                    {
                        selectOnly(ppPos);
                        m_anchor = m_focus = ppPos;
                    }
                }
                else
                {
                    pp.select_all(false);
                    pp.select_entry(ppPos, true);
                    pp.set_focus(ppPos);
                }
            }
            m_lastPlayPl = ppIdx;
            m_lastPlayPos = ppPos;
        }
        else
            m_lastPlayPl = m_lastPlayPos = -1;

        if (m_position != prevPos || m_activePl != prevPl)
        {
            ensureCurrentVisible();
            changed = true;
        }

        if (playing != m_playing || paused != m_paused || time / 250 != m_time / 250 ||
            length != m_length || volume != m_volume || shuffle != m_shuffle ||
            repeat != m_repeat)
            changed = true;

        if (m_volumeKnown && volume != m_volume)
            m_volReadoutUntil = m_clock.elapsed() + 1000;
        m_volumeKnown = true;

        const bool readoutShown = m_clock.elapsed() < m_volReadoutUntil;
        if (readoutShown != m_volReadoutShown)
        {
            m_volReadoutShown = readoutShown;
            changed = true;
        }

        m_playing = playing;
        m_paused = paused;
        m_time = time;
        m_length = length;
        m_volume = volume;
        m_shuffle = shuffle;
        m_repeat = repeat;

        QFontMetrics fm(m_tickerFont);
        if (fm.horizontalAdvance(m_ticker) > tickerRect().width() && ++m_scrollDiv >= 2)
        {
            m_scrollDiv = 0;
            m_scroll++;
            changed = true;
        }

        if (autoScroll())
            changed = true;

        if (changed)
            update();
    }

    /* ---- layout helpers (logical pixels) ------------------------------------ */

    int BentoWidget::leftColumnWidth() const
    {
        return m_w - m_plW;
    }

    int BentoWidget::playerWidth() const
    {
        // player band fills the left column minus the art column
        return qMax(PLAYER_W, leftColumnWidth() - ART_COL_W);
    }

    QRect BentoWidget::artRect() const
    {
        return QRect(leftColumnWidth() - ART_MARGIN - ART_BOX, ART_Y, ART_BOX, ART_BOX);
    }

    int BentoWidget::maxPlaylistWidth() const
    {
        // allow the splitter to travel almost to the left edge
        return qMax(PL_MIN_W, m_w - LEFT_MIN_W);
    }

    QRect BentoWidget::playerArea() const
    {
        return QRect(0, 0, playerWidth(), PLAYER_H);
    }

    QRect BentoWidget::playlistArea() const
    {
        return QRect(m_w - m_plW, -TOP_PAD, m_plW, m_h + TOP_PAD); // runs up over the top band
    }

    QRect BentoWidget::tabArea() const
    {
        int leftW = leftColumnWidth();
        return QRect(0, PLAYER_H, leftW, TAB_H);
    }

    QRect BentoWidget::videoArea() const
    {
        int leftW = leftColumnWidth();
        int y = PLAYER_H + TAB_H;
        int h = m_h - y - VID_FOOTER;
        if (h < 0)
            h = 0;
        return QRect(0, y, leftW, h);
    }

    QRect BentoWidget::splitterRect() const
    {
        return QRect(m_w - m_plW - 3, -TOP_PAD, 6, m_h + TOP_PAD);
    }

    QRect BentoWidget::btnRect(int id) const
    {
        const int dx = playerWidth() - GW;
        switch (id)
        {
            case B_PREV:
            case B_PLAY:
            case B_PAUSE:
            case B_STOP:
            case B_NEXT:
                return kBtnRect[id];
            case B_EJECT:
            case B_SHUFFLE:
            case B_REPEAT:
            case B_MUTE:
            case B_BOLT:
                return QRect(kBtnRect[id].x() + dx, kBtnRect[id].y(),
                             kBtnRect[id].width(), kBtnRect[id].height());
            default:
                return QRect();
        }
    }

    QRect BentoWidget::volSliderRect() const
    {
        const int dx = playerWidth() - GW;
        return QRect(kVolSlider.x() + dx, kVolSlider.y(), kVolSlider.width(),
                     kVolSlider.height());
    }

    QRect BentoWidget::seekSliderRect() const
    {
        return QRect(3, 59, qMax(40, playerWidth() - 4), 10);
    }

    QRect BentoWidget::tickerRect() const
    {
        // Under horizontal stretch, grow the ticker with the player; under shrink
        // the painter is scaled to skin coords so keep the original box.
        const int pw = playerWidth();
        int right; // exclusive right edge of the ticker
        if (pw < PLAYER_W)
            right = kTickerBox.x() + kTickerBox.width();
        else
            // right edge lines up with the end of STEREO (x = 220 in skin coords)
            right = kTickerBox.x() + qMax(40, pw - GW + 220 - kTickerBox.x());

        // The time display grows with its digits (10+ minutes, "-" prefix, hours):
        // push the ticker's left edge right so the two never overlap.
        const int left = qMax(kTickerBox.x(), timeEndX() + 6);
        return QRect(left, kTickerBox.y(), qMax(20, right - left), kTickerBox.height());
    }

    int BentoWidget::listTop() const
    {
        return playlistArea().y() + PL_TAB_H + PL_HEADER;
    }

    /* The rows live in a viewport that starts PL_HEADER under the tabs and ends
     * PL_BOTTOM above the footer.  Its height is rarely a whole number of rows, so
     * when the list is scrolled to its end the rows are pinned to the bottom edge:
     * the last row then ends exactly PL_BOTTOM above the footer (the same gap as
     * under the tabs) and the first row drawn starts partly above the viewport. */
    int BentoWidget::viewportHeight() const
    {
        return playlistArea().height() - PL_TAB_H - PL_HEADER - PL_FOOTER - PL_BOTTOM;
    }

    bool BentoWidget::pinnedToBottom() const
    {
        const int n = m_entries.size();
        const int vis = playlistRowsVisible();
        const int rem = viewportHeight() - vis * plRow();
        return rem > 0 && n > vis && m_plScroll >= n - vis;
    }

    int BentoWidget::firstRow() const
    {
        return pinnedToBottom() ? m_plScroll - 1 : m_plScroll;
    }

    int BentoWidget::firstRowOffset() const
    {
        if (!pinnedToBottom())
            return 0;
        const int rem = viewportHeight() - playlistRowsVisible() * plRow();
        return -(plRow() - rem);
    }

    int BentoWidget::playlistRowsVisible() const
    {
        int h = playlistArea().height() - PL_TAB_H - PL_HEADER - PL_FOOTER - PL_BOTTOM;
        return qMax(1, h / plRow());
    }

    QRect BentoWidget::plTabBarRect() const
    {
        QRect area = playlistArea();
        return QRect(area.x(), area.y(), area.width(), PL_TAB_H);
    }

    QRect BentoWidget::prefsButtonRect() const
    {
        QRect area = videoArea();
        QRect foot(area.x(), m_h - VID_FOOTER, area.width(), VID_FOOTER);
        return QRect(foot.right() - 28, foot.y() + 3, 20, 18);
    }

    QRect BentoWidget::plTabRect(int index) const
    {
        if (index < 0 || index >= m_plTitles.size())
            return QRect();

        QFontMetrics fm(m_tabFont);
        int x = playlistArea().x() + PL_TAB_X0 - m_plTabScroll;
        for (int i = 0; i < index; i++)
        {
            int tw = fm.horizontalAdvance(m_plTitles[i]) + 16;
            x += tw + 2;
        }
        int tw = fm.horizontalAdvance(m_plTitles[index]) + 16;
        return QRect(x, playlistArea().y() + 2, tw, PL_TAB_H - 3);
    }

    int BentoWidget::plTabAt(const QPoint & pt) const
    {
        if (!plTabBarRect().contains(pt) || plusButtonRect().contains(pt))
            return -1;
        for (int i = 0; i < m_plTitles.size(); i++)
            if (plTabRect(i).contains(pt))
                return i;
        return -1;
    }

    int BentoWidget::tabSlotAt(int x) const
    {
        const int n = m_plTitles.size();
        x = qBound(playlistArea().x() + PL_TAB_X0, x, plusButtonRect().left());
        for (int i = 0; i < n; i++)
            if (plTabRect(i).center().x() > x)
                return i;
        return n;
    }

    void BentoWidget::cancelTabDrag()
    {
        if (m_drag == Drag::TabReorder)
        {
            m_drag = Drag::None;
            unsetCursor();
        }
        m_pressTab = -1;
        m_tabDropSlot = -1;
    }

    QRect BentoWidget::plusButtonRect() const
    {
        const QRect bar = plTabBarRect();
        return QRect(bar.right() - 21, bar.y() + 1, 20, PL_TAB_H - 2);
    }

    int BentoWidget::tabsTotalWidth() const
    {
        QFontMetrics fm(m_tabFont);
        int w = PL_TAB_X0;
        for (const QString & t : m_plTitles)
            w += fm.horizontalAdvance(t) + 16 + 2;
        return w;
    }

    int BentoWidget::tabsAvailWidth() const
    {
        return plusButtonRect().left() - playlistArea().x() - PL_TAB_X0;
    }

    void BentoWidget::ensureTabVisible(int index)
    {
        if (index < 0 || index >= m_plTitles.size())
            return;

        QFontMetrics fm(m_tabFont);
        int x = PL_TAB_X0;
        for (int i = 0; i < index; i++)
            x += fm.horizontalAdvance(m_plTitles[i]) + 16 + 2;
        const int w = fm.horizontalAdvance(m_plTitles[index]) + 16;
        const int avail = tabsAvailWidth();

        if (x < m_plTabScroll + PL_TAB_X0)
            m_plTabScroll = x - PL_TAB_X0;
        else if (x + w > m_plTabScroll + avail)
            m_plTabScroll = x + w - avail;
        m_plTabScroll = qMax(0, m_plTabScroll);
    }

    QRect BentoWidget::scrollbarRect() const
    {
        QRect area = playlistArea();
        // 2px gap on top, bottom, and right; flush on the left (toward the list)
        int top = area.y() + PL_TAB_H + 2;
        int h = area.height() - PL_TAB_H - PL_FOOTER - 4;
        return QRect(area.x() + area.width() - SCROLL_W - 2, top, SCROLL_W, h);
    }

    QRect BentoWidget::thumbTrackRect() const
    {
        // 2px of spacing above and below the thumb's travel range
        return scrollbarRect().adjusted(0, 2, 0, -2);
    }

    QRect BentoWidget::thumbRect() const
    {
        int n = m_entries.size();
        int vis = playlistRowsVisible();
        QRect track = thumbTrackRect();
        if (n <= vis) // nothing to scroll: the thumb fills the whole track
            return QRect(track.x() + 2, track.y(), track.width() - 4, track.height());

        int thumbH = qMax(20, track.height() * vis / n);
        int maxScroll = n - vis;
        int y = track.y() + (maxScroll > 0
        ? (track.height() - thumbH) * m_plScroll / maxScroll
        : 0);
        return QRect(track.x() + 2, y, track.width() - 4, thumbH);
    }

    int BentoWidget::entryAtY(int y) const
    {
        const int listTop = this->listTop();
        if (y < listTop || y >= m_h - PL_FOOTER - PL_BOTTOM)
            return -1;
        int row = (y - listTop - firstRowOffset()) / plRow() + firstRow();
        if (row < 0 || row >= m_entries.size())
            return -1;
        return row;
    }

    /* ---- painting ----------------------------------------------------------- */

    void BentoWidget::drawGrid(QPainter & p, const QPixmap & src, int sx, int sy,
                               int lw, int mw, int rw, int h, int x, int y, int w)
    {
        p.drawPixmap(x, y, src, sx, sy, lw, h);
        p.drawPixmap(x + w - rw, y, src, sx + lw + mw, sy, rw, h);
        if (w - lw - rw > 0)
            p.drawPixmap(QRect(x + lw, y, w - lw - rw, h), src,
                         QRect(sx + lw, sy, mw, h));
    }

    void BentoWidget::buttonSprite(int id, int state, int & sx, int & sy) const
    {
        switch (id)
        {
            case B_PREV:  sx = 0;  sy = 103 + 24 * state; break;
            case B_PLAY:  sx = 26; sy = 103 + 24 * state; break;
            case B_PAUSE: sx = 48; sy = 103 + 24 * state; break;
            case B_STOP:  sx = 70; sy = 103 + 24 * state; break;
            case B_NEXT:  sx = 92; sy = 103 + 24 * state; break;
            case B_EJECT: sx = 25 + 25 * state; sy = 175; break;
            case B_SHUFFLE: sx = 32 * state; sy = 209 + (m_shuffle ? 17 : 0); break;
            case B_REPEAT:  sx = 27 * state; sy = 243 + (m_repeat ? 17 : 0); break;
            case B_MUTE: sx = 25 * state + (m_premute >= 0 ? 75 : 0); sy = 55; break;
            case B_BOLT: sx = 180; sy = 184 + 37 * state; break;
            default: sx = sy = 0;
        }
    }

    QString BentoWidget::timeString() const
    {
        int secs = (m_playing ? m_time : 0) / 1000;
        bool minus = false;
        if (m_remaining && m_playing && m_length > 0)
        {
            secs = qMax(0, (m_length - m_time) / 1000);
            minus = true;
        }

        QString text = QString("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0'));
        if (minus)
            text.prepend('-');
        return text;
    }

    int BentoWidget::timeEndX() const
    {
        int x = kTimeRect.x();
        for (QChar ch : timeString())
            x += (ch == ':') ? 6 : 15;
        return x;
    }

    void BentoWidget::drawTime(QPainter & p)
    {
        const QString text = timeString();

        int x = kTimeRect.x();
        for (QChar ch : text)
        {
            int idx = (ch == ':') ? 10 : (ch == '-') ? 11 : ch.digitValue();
            if (idx < 0)
                continue;
            p.drawPixmap(x, kTimeRect.y(), m_digits[idx]);
            x += (ch == ':') ? 6 : 15;
        }
    }

    void BentoWidget::drawTicker(QPainter & p)
    {
        QRect box = tickerRect();
        p.save();
        p.setClipRect(box);
        p.setFont(m_tickerFont);
        p.setPen(kPlText); // same colour as playlist entries

        QFontMetrics fm(m_tickerFont);

        // Transient volume readout replaces the title; m_scroll is untouched.
        const bool showVol = m_clock.elapsed() < m_volReadoutUntil;
        const QString volText = QString("Volume: %1%").arg(m_volume);
        const QString & text = showVol ? volText : m_ticker;
        int tw = fm.horizontalAdvance(text);

        // Single baseline for both fitted and scrolling text so vertical
        // position never jumps when the title starts/stops scrolling.
        // 1px optical nudge up within the LCD strip.
        const int baseY = box.top() + (box.height() + fm.ascent() - fm.descent()) / 2 - 2;
        if (showVol || tw <= box.width())
        {
            p.drawText(box.right() + 1 - tw, baseY, text); // right-aligned
        }
        else
        {
            const int gap = 40;
            int off = m_scroll % (tw + gap);
            p.drawText(box.left() - off, baseY, text);
            p.drawText(box.left() - off + tw + gap, baseY, text);
        }
        p.restore();
    }

    void BentoWidget::drawStatus(QPainter & p, int dx)
    {
        const int x = 103 + dx, y = 20;
        if (!m_playing)
            p.fillRect(x, y, 5, 5, kStatusColor);
        else if (m_paused)
        {
            p.fillRect(x, y, 2, 6, kStatusColor);
            p.fillRect(x + 3, y, 2, 6, kStatusColor);
        }
        else
        {
            static const int widths[6] = {1, 2, 3, 3, 2, 1};
            for (int r = 0; r < 6; r++)
                p.fillRect(x, y + r, widths[r], 1, kStatusColor);
        }
    }

    void BentoWidget::drawSongInfo(QPainter & p, int dx)
    {
        drawStatus(p, dx);
        if (!m_playing || m_bitrate <= 0)
            return;

        QByteArray br = QByteArray::number(m_bitrate / 1000);
        QByteArray sr = QByteArray::number(m_samplerate / 1000);

        drawPixText(p, dx + 127 - 5 * br.size(), 20, br.constData(), kInfoBright);
        drawPixText(p, dx + 129, 20, "KBPS", kInfoDim);
        drawPixText(p, dx + 160 - 5 * sr.size(), 20, sr.constData(), kInfoBright);
        drawPixText(p, dx + 162, 20, "KHZ", kInfoDim);

        if (m_channels >= 2)
        {
            for (int ring = 0; ring < 2; ring++)
            {
                int rx = dx + 180 + ring * 4;
                p.fillRect(rx + 1, 20, 2, 1, kInfoBright);
                p.fillRect(rx, 21, 1, 3, kInfoBright);
                p.fillRect(rx + 3, 21, 1, 3, kInfoBright);
                p.fillRect(rx + 1, 24, 2, 1, kInfoBright);
            }
            drawPixText(p, dx + 190, 20, "STEREO", kInfoDim);
        }
        else if (m_channels == 1)
            drawPixText(p, dx + 190, 20, "MONO", kInfoDim);
    }

    void BentoWidget::drawPlayer(QPainter & p)
    {
        const int pw = playerWidth();
        const int dx = pw - GW;

        p.fillRect(0, 0, pw, PLAYER_H, playerGradient());

        if (!m_ok)
        {
            p.setPen(Qt::white);
            p.drawText(QRect(6, 6, qMax(40, pw - 12), PLAYER_H - 12), Qt::TextWordWrap,
                       m_error);
            return;
        }

        // When the left column is narrower than the original skin, scale down
        // horizontally so controls stay usable; otherwise stretch the display.
        const bool shrink = pw < PLAYER_W;
        p.save();
        if (shrink)
            p.scale(double(pw) / PLAYER_W, 1.0);

        const int drawW = shrink ? PLAYER_W : pw;
        const int drawDx = shrink ? 0 : dx;
        const int dispW = drawW - 4;

        drawGrid(p, m_sheet, 0, 0, 80, 10, 132, 54, 3, 0, dispW);
        drawGrid(p, m_sheet, 0, 326, 80, 10, 132, 54, 3, 0, dispW);

        // time / ticker / songinfo use original coords; under shrink the scale handles it
        if (shrink)
        {
            // draw with original geometry (scale applied above)
            drawTime(p);
            drawTicker(p); // uses kTickerBox coords under the shrink transform
            drawSongInfo(p, 0);

            p.drawPixmap(GW - 78, 34, m_sheet, 0, 69, 71, 8);
            p.drawPixmap(GW - 112, 40, m_sheet, 150, 55, 25, 14);
            drawGrid(p, m_sheet, 175, 56, 5, 15, 5, 5, GW - 80, 44, 78);

            double vol = qBound(0, m_volume, 100) / 100.0;
            int volThumbX = kVolSlider.x() + int(vol * (kVolSlider.width() - kVolThumbW));
            {
                int fw = volThumbX + kVolThumbW / 2 - (GW - 78);
                p.save();
                p.setClipRect(QRect(GW - 78, 46, qMax(0, fw), 2));
                drawGrid(p, m_light, 0, 0, 4, 8, 4, 2, GW - 78, 46, 76);
                p.restore();
            }

            drawGrid(p, m_sheet, 175, 56, 5, 15, 5, 5, 4, 60, GW - 6);
            if (m_playing && m_length > 0)
            {
                double frac = (m_drag == Drag::Seek) ? m_dragFrac
                : qBound(0.0, double(m_time) / m_length, 1.0);
                int thumbX = kSeekSlider.x() + int(frac * (kSeekSlider.width() - kSeekThumbW));
                p.save();
                p.setClipRect(QRect(6, 62, qMax(0, thumbX + kSeekThumbW / 2 - 6), 2));
                drawGrid(p, m_light, 0, 0, 4, 8, 4, 2, 6, 62, GW - 8);
                p.restore();
                p.drawPixmap(thumbX, 59, m_sheet, 122, 69, kSeekThumbW, 10);
            }

            static const int wells[][6] = {
                {0, 79, 26, 24, 0, 70},   {26, 79, 22, 24, 25, 70}, {48, 79, 22, 24, 46, 70},
                {70, 79, 22, 24, 67, 70}, {92, 79, 26, 24, 88, 70},
                {0, 175, 25, 17, GW - 112, 73}, {0, 192, 32, 17, GW - 83, 73},
                {32, 192, 24, 17, GW - 51, 73},
            };
            for (const auto & w : wells)
                p.drawPixmap(w[4], w[5], m_sheet, w[0], w[1], w[2], w[3]);

            for (int id = 0; id < B_COUNT; id++)
            {
                int state = (m_down == id && m_hover == id) ? 2 : (m_hover == id ? 1 : 0);
                int sx, sy;
                buttonSprite(id, state, sx, sy);
                const QRect & r = kBtnRect[id];
                p.drawPixmap(r.x(), r.y(), m_sheet, sx, sy, r.width(), r.height());
            }

            double vol2 = qBound(0, m_volume, 100) / 100.0;
            int vtx = kVolSlider.x() + int(vol2 * (kVolSlider.width() - kVolThumbW));
            p.drawPixmap(vtx, 43, m_sheet, 71 + (m_drag == Drag::Volume ? 34 : 0), 69,
                         kVolThumbW, 10);

            if (m_premute >= 0)
                p.drawPixmap(GW - 115, 33, m_glow, 0, 0, 28, 28);
            if (m_shuffle)
                p.drawPixmap(GW - 84, 66, m_glow, 28, 0, 34, 31);
            if (m_repeat)
                p.drawPixmap(GW - 56, 66, m_glow, 62, 0, 35, 30);
        }
        else
        {
            drawTime(p);
            drawTicker(p);
            drawSongInfo(p, drawDx);

            p.drawPixmap(GW - 78 + drawDx, 34, m_sheet, 0, 69, 71, 8);
            p.drawPixmap(GW - 112 + drawDx, 40, m_sheet, 150, 55, 25, 14);
            drawGrid(p, m_sheet, 175, 56, 5, 15, 5, 5, GW - 80 + drawDx, 44, 78);

            QRect volR = volSliderRect();
            double vol = qBound(0, m_volume, 100) / 100.0;
            int volThumbX = volR.x() + int(vol * (volR.width() - kVolThumbW));
            {
                int trackLeft = GW - 78 + drawDx;
                int fw = volThumbX + kVolThumbW / 2 - trackLeft;
                p.save();
                p.setClipRect(QRect(trackLeft, 46, qMax(0, fw), 2));
                drawGrid(p, m_light, 0, 0, 4, 8, 4, 2, trackLeft, 46, 76);
                p.restore();
            }

            QRect seekR = seekSliderRect();
            drawGrid(p, m_sheet, 175, 56, 5, 15, 5, 5, seekR.x() + 1, 60, seekR.width() - 2);
            if (m_playing && m_length > 0)
            {
                double frac = (m_drag == Drag::Seek) ? m_dragFrac
                : qBound(0.0, double(m_time) / m_length, 1.0);
                int thumbX = seekR.x() + int(frac * (seekR.width() - kSeekThumbW));
                p.save();
                p.setClipRect(QRect(seekR.x() + 3, 62,
                                    qMax(0, thumbX + kSeekThumbW / 2 - (seekR.x() + 3)), 2));
                drawGrid(p, m_light, 0, 0, 4, 8, 4, 2, seekR.x() + 3, 62, seekR.width() - 6);
                p.restore();
                p.drawPixmap(thumbX, 59, m_sheet, 122, 69, kSeekThumbW, 10);
            }

            static const int wellsLeft[][6] = {
                {0, 79, 26, 24, 0, 70},   {26, 79, 22, 24, 25, 70}, {48, 79, 22, 24, 46, 70},
                {70, 79, 22, 24, 67, 70}, {92, 79, 26, 24, 88, 70},
            };
            for (const auto & w : wellsLeft)
                p.drawPixmap(w[4], w[5], m_sheet, w[0], w[1], w[2], w[3]);

            p.drawPixmap(GW - 112 + drawDx, 73, m_sheet, 0, 175, 25, 17);
            p.drawPixmap(GW - 83 + drawDx, 73, m_sheet, 0, 192, 32, 17);
            p.drawPixmap(GW - 51 + drawDx, 73, m_sheet, 32, 192, 24, 17);

            for (int id = 0; id < B_COUNT; id++)
            {
                int state = (m_down == id && m_hover == id) ? 2 : (m_hover == id ? 1 : 0);
                int sx, sy;
                buttonSprite(id, state, sx, sy);
                QRect r = btnRect(id);
                p.drawPixmap(r.x(), r.y(), m_sheet, sx, sy, r.width(), r.height());
            }

            p.drawPixmap(volThumbX, 43, m_sheet, 71 + (m_drag == Drag::Volume ? 34 : 0), 69,
                         kVolThumbW, 10);

            if (m_premute >= 0)
                p.drawPixmap(GW - 115 + drawDx, 33, m_glow, 0, 0, 28, 28);
            if (m_shuffle)
                p.drawPixmap(GW - 84 + drawDx, 66, m_glow, 28, 0, 34, 31);
            if (m_repeat)
                p.drawPixmap(GW - 56 + drawDx, 66, m_glow, 62, 0, 35, 30);
        }

        p.restore();
    }

    void BentoWidget::drawTabs(QPainter & p)
    {
        QRect area = tabArea();
        // Match the tab strip to the bottom-most colour of the player gradient.
        p.fillRect(area, QColor(0x2C, 0x32, 0x35));
        p.fillRect(area.x(), area.bottom(), area.width(), 1, kChromeEdge);

        p.setFont(m_tabFont);
        QFontMetrics fm(m_tabFont);
        int x = area.x() + 4;

        for (int i = 0; i < kTabCount; i++)
        {
            QString name = kTabNames[i];
            int tw = fm.horizontalAdvance(name) + 16;
            QRect tab(x, area.y() + 2, tw, area.height() - 3);
            bool active = (i == m_activeTab);

            if (active)
            {
                p.fillRect(tab, kTabActive);
                p.setPen(kChromeEdge);
                p.drawRect(tab.adjusted(0, 0, -1, 0));
                p.setPen(kTabActive);
                p.drawLine(tab.left() + 1, tab.bottom(), tab.right() - 1, tab.bottom());
            }

            p.setPen(active ? QColor(230, 230, 230) : kTabText);
            p.drawText(tab, Qt::AlignCenter, name);
            x += tw + 2;
        }
    }

    void BentoWidget::drawArt(QPainter & p)
    {
        const int left = leftColumnWidth() - ART_COL_W;

        p.fillRect(left, 0, ART_COL_W, PLAYER_H, playerGradient());

        const QRect frame = artRect();
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(kChromeEdge, 1.0));
        p.setBrush(kChromeDark);
        p.drawRoundedRect(QRectF(frame).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.restore();

        const QRect inner(frame.x() + ART_INSET, frame.y() + ART_INSET, ART_IMG, ART_IMG);
        p.fillRect(inner, Qt::black);

        if (!m_art.isNull())
        {
            const QSize sz = m_art.size() / m_art.devicePixelRatio();
            p.drawPixmap(inner.x() + (ART_IMG - sz.width()) / 2,
                         inner.y() + (ART_IMG - sz.height()) / 2, m_art);
            return;
        }

        // no cover: dim music note (ellipse head, stem, flag)
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPoint c = inner.center();
        p.setPen(Qt::NoPen);
        p.setBrush(kInfoDim);
        p.drawEllipse(QPointF(c.x() - 5, c.y() + 14), 9.0, 7.0);
        p.drawRect(c.x() + 2, c.y() - 20, 3, 34);
        QPainterPath flag;
        flag.moveTo(c.x() + 5, c.y() - 20);
        flag.cubicTo(c.x() + 16, c.y() - 16, c.x() + 18, c.y() - 6, c.x() + 12, c.y() + 2);
        flag.cubicTo(c.x() + 15, c.y() - 6, c.x() + 11, c.y() - 11, c.x() + 5, c.y() - 12);
        flag.closeSubpath();
        p.drawPath(flag);
        p.restore();
    }

    void BentoWidget::drawNullsoftIcon(QPainter & p, int cx, int cy)
    {
        // stylised film-strip + "NULLSOFT VIDEO" badge, similar to the target
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(kVidIcon, 2.0));
        p.setBrush(Qt::NoBrush);

        const int w = 64;
        const int h = 48;
        QRect frame(cx - w / 2, cy - h / 2 - 6, w, h);

        // outer rounded rect (film frame)
        p.drawRoundedRect(frame, 4, 4);

        // sprocket holes left/right
        p.setBrush(kVidIcon);
        for (int i = 0; i < 4; i++)
        {
            int yy = frame.top() + 6 + i * 10;
            p.drawRect(frame.left() + 3, yy, 4, 5);
            p.drawRect(frame.right() - 6, yy, 4, 5);
        }

        // inner screen
        QRect inner = frame.adjusted(10, 6, -10, -6);
        p.setBrush(Qt::NoBrush);
        p.drawRect(inner);

        // tiny dog / logo placeholder (simple silhouette)
        p.setBrush(kVidIcon);
        p.setPen(Qt::NoPen);
        int lx = inner.center().x();
        int ly = inner.center().y();
        p.drawEllipse(QPoint(lx - 4, ly - 2), 5, 5); // head
        p.drawEllipse(QPoint(lx + 4, ly + 2), 7, 5); // body

        // label under the frame
        p.setPen(kVidIcon);
        p.setFont(m_tabFont);
        QFont f = m_tabFont;
        f.setBold(true);
        f.setPixelSize(10);
        p.setFont(f);
        p.drawText(QRect(cx - 50, frame.bottom() + 4, 100, 16),
                   Qt::AlignHCenter | Qt::AlignTop, "NULLSOFT VIDEO");

        p.restore();
    }

    void BentoWidget::drawVideo(QPainter & p)
    {
        QRect area = videoArea();
        p.fillRect(area, kVidBg);

        // centred Nullsoft Video placeholder
        if (area.width() > 80 && area.height() > 80)
            drawNullsoftIcon(p, area.center().x(), area.center().y());

        // video footer chrome
        QRect foot(area.x(), m_h - VID_FOOTER, area.width(), VID_FOOTER);
        p.fillRect(foot, kChromeBg);
        p.fillRect(foot.x(), foot.y(), foot.width(), 1, kChromeEdge);

        // left decorative buttons (1x, 2x, TV) — visual only for now
        p.setFont(m_tabFont);
        const char * labels[] = {"1x", "2x", "TV"};
        int bx = foot.x() + 8;
        for (int i = 0; i < 3; i++)
        {
            QRect b(bx, foot.y() + 3, 22, 18);
            p.setPen(kChromeEdge);
            p.setBrush(kTabInactive);
            p.drawRoundedRect(b, 2, 2);
            p.setPen(kTabText);
            p.drawText(b, Qt::AlignCenter, labels[i]);
            bx += 26;
        }

        // wrench = open Audacious preferences
        QRect wr = prefsButtonRect();
        p.setPen(kChromeEdge);
        p.setBrush(kTabInactive);
        p.drawRoundedRect(wr, 2, 2);
        // simple wrench glyph
        p.setPen(QPen(kTabText, 1.5));
        p.setBrush(Qt::NoBrush);
        int cx = wr.center().x();
        int cy = wr.center().y();
        p.drawEllipse(QPoint(cx - 3, cy - 3), 3, 3);
        p.drawLine(cx - 1, cy - 1, cx + 5, cy + 5);
        p.drawLine(cx + 3, cy + 3, cx + 6, cy + 2);
        p.drawLine(cx + 3, cy + 3, cx + 4, cy + 6);
    }

    void BentoWidget::drawPlaylist(QPainter & p)
    {
        QRect area = playlistArea();
        p.fillRect(area, kPlBg);

        // left edge
        p.fillRect(area.x(), area.y(), 1, area.height(), kPlDivider);

        // ---- playlist selector tabs ----
        QRect tabBar = plTabBarRect();
        p.fillRect(tabBar, playerGradient());
        p.fillRect(tabBar.x(), tabBar.bottom(), tabBar.width(), 1, kChromeEdge);

        p.setFont(m_tabFont);
        QFontMetrics tabFm(m_tabFont);
        // clip starts at the pane edge so the first tab's left border shows
        p.setClipRect(QRect(tabBar.x(), tabBar.y(),
                            plusButtonRect().left() - tabBar.x(), tabBar.height()));

        for (int i = 0; i < m_plTitles.size(); i++)
        {
            QRect tab = plTabRect(i);
            if (tab.right() < tabBar.left() || tab.left() > tabBar.right())
                continue;

            bool active = (i == m_activePl);
            bool hover = m_extDrag && i == m_plTabHover; // file-drop target only, no mouse hover

            if (active)
            {
                p.fillRect(tab, kTabActive);
                p.setPen(kChromeEdge);
                p.drawRect(tab.adjusted(0, 0, -1, 0));
                p.setPen(kTabActive);
                p.drawLine(tab.left() + 1, tab.bottom(), tab.right() - 1, tab.bottom());
            }
            else if (hover)
            {
                p.fillRect(tab, kTabInactive);
            }

            p.setPen(active ? QColor(230, 230, 230) : kTabText);
            p.drawText(tab, Qt::AlignCenter, m_plTitles[i]);
        }
        if (m_drag == Drag::TabReorder && m_tabDropSlot >= 0 && !m_plTitles.isEmpty())
        {
            const int n = m_plTitles.size();
            const QRect ref = plTabRect(qMin(m_tabDropSlot, n - 1));
            const int x = (m_tabDropSlot < n) ? qMax(ref.left() - 2, tabBar.x()) : ref.right() + 1;
            p.fillRect(x, ref.y(), 2, ref.height(), kPlDrop);
        }
        if (m_plTabScroll > 0) // scrolled tabs must not paint over the pane's left edge
            p.fillRect(tabBar.x(), tabBar.y(), 1, PL_TAB_H - 1, kPlDivider);
        p.setClipping(false);

        // "+" button: new playlist
        {
            const QRect plus = plusButtonRect();
            p.setPen(kTabText);
            const int cx = plus.x() + plus.width() / 2;
            const int cy = plus.y() + plus.height() / 2;
            p.drawLine(cx - 4, cy, cx + 4, cy);
            p.drawLine(cx, cy - 4, cx, cy + 4);
        }

        // ---- entries ----
        p.setFont(m_plFont);
        QFontMetrics fm(m_plFont);

        int vis = playlistRowsVisible();
        int n = m_entries.size();
        int textW = area.width() - PL_PAD_X * 2 - SCROLL_W - 4;
        const int listTop = this->listTop();
        Playlist activePl = Playlist::active_playlist();
        // only the playlist holding the playing song shows the "playing" style
        const bool showCurrent = (Playlist::playing_playlist() == activePl);

        // The list viewport ends PL_BOTTOM above the footer, mirroring the gap under
        // the tabs.  Clip to it and draw one extra row so a partly visible last row
        // is cut cleanly at that edge.
        const int viewBottom = area.bottom() - PL_FOOTER + 1 - PL_BOTTOM;
        p.setClipRect(QRect(area.x(), listTop, area.width(), qMax(0, viewBottom - listTop)));

        for (int i = 0; i <= vis; i++)
        {
            int idx = firstRow() + i;
            if (idx >= n)
                break;

            int y = listTop + firstRowOffset() + i * plRow();
            const PlEntry & e = m_entries[idx];

            bool current = showCurrent && idx == m_position;
            bool hover = (idx == m_plHover);
            const int rowW = area.width() - SCROLL_W - 4;

            if (current)
                p.fillRect(area.x() + 1, y, rowW, plRow(), QColor(30, 28, 18));
            else if (e.selected)
                p.fillRect(area.x() + 1, y, rowW, plRow(), kPlSelected);
            else if (hover && m_drag == Drag::None)
                p.fillRect(area.x() + 1, y, rowW, plRow(), QColor(22, 22, 18));

            QColor textColor = current ? kPlCurrentText : kPlText;

            const int queuePos = shimQueuePos(activePl, idx);
            const int markerW = (queuePos >= 0) ? 30 : 0;

            QString label = QString("%1. %2").arg(idx + 1).arg(e.title);
            QString elided = fm.elidedText(label, Qt::ElideRight, textW - 40 - markerW);

            p.setPen(textColor);
            p.drawText(area.x() + PL_PAD_X, y, textW - 40 - markerW, plRow(),
                       Qt::AlignLeft | Qt::AlignVCenter, elided);

            if (queuePos >= 0)
            {
                p.setPen(kPlCurrent);
                p.drawText(area.x() + area.width() - SCROLL_W - 6 - 40 - 30, y, 30, plRow(),
                           Qt::AlignRight | Qt::AlignVCenter,
                           QString("[%1]").arg(queuePos + 1));
            }

            if (!e.length.isEmpty())
            {
                p.setPen(current ? kPlCurrentText : kPlText);
                p.drawText(area.x() + area.width() - SCROLL_W - 6 - 40, y, 40, plRow(),
                           Qt::AlignRight | Qt::AlignVCenter, e.length);
            }
        }

        // drag-select band
        if (m_drag == Drag::Rubber)
        {
            const int base = listTop + firstRowOffset() - firstRow() * plRow();
            const int y0 = base + m_rubberY0;
            const int y1 = base + m_rubberY1;
            const QRect band(QPoint(qMin(m_rubberX0, m_rubberX1), qMin(y0, y1)),
                             QPoint(qMax(m_rubberX0, m_rubberX1), qMax(y0, y1)));
            p.fillRect(band, QColor(255, 255, 255, 28));
            p.setPen(QColor(170, 170, 185, 170));
            p.drawRect(band);
        }

        // drop / reorder insertion marker
        if (m_dropRow >= 0 && (m_drag == Drag::PlReorder || m_extDrag))
        {
            int rel = m_dropRow - firstRow();
            if (rel >= 0 && rel <= vis + 1)
                p.fillRect(area.x() + 1, listTop + firstRowOffset() + rel * plRow() - 1,
                           area.width() - SCROLL_W - 4, 2, kPlDrop);
        }

        p.setClipping(false); // end of the list viewport clip

        // scrollbar: always shown, even when the whole list fits
        {
            QRect track = scrollbarRect();
            p.fillRect(track, kPlScrollBg);
            QRect thumb = thumbRect();
            if (thumb.isValid())
                p.fillRect(thumb, kPlScrollThumb);
        }

        // footer
        int fy = area.bottom() - PL_FOOTER + 1;
        p.fillRect(area.x(), fy, area.width(), PL_FOOTER, kPlFooterBg);
        p.fillRect(area.x(), fy, area.width(), 1, kPlDivider);

        p.setPen(kPlDim);
        p.setFont(m_footerFont);

        QString totalStr;
        if (m_totalLength > 0)
        {
            int ts = m_totalLength / 1000;
            int h = ts / 3600;
            int m = (ts % 3600) / 60;
            int s = ts % 60;
            if (h > 0)
                totalStr = QString("%1:%2:%3")
                .arg(h)
                .arg(m, 2, 10, QChar('0'))
                .arg(s, 2, 10, QChar('0'));
            else
                totalStr = QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
        }
        else
            totalStr = "0:00";

        QString footer = QString("0:00 / %1").arg(totalStr);
        p.drawText(area.x() + PL_PAD_X, fy, area.width() - PL_PAD_X * 2, PL_FOOTER,
                   Qt::AlignRight | Qt::AlignVCenter, footer);

        QString countStr = QString("%1 item%2").arg(n).arg(n == 1 ? "" : "s");
        p.drawText(area.x() + PL_PAD_X, fy, area.width() / 2, PL_FOOTER,
                   Qt::AlignLeft | Qt::AlignVCenter, countStr);
    }

    void BentoWidget::paintEvent(QPaintEvent *)
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);

        // overall dark fill
        p.fillRect(rect(), kChromeDark);
        // The player column owns its top inset; the playlist starts flush
        // beneath the OS title bar.
        p.fillRect(QRect(0, 0, leftColumnWidth(), TOP_PAD), QColor(0x2E, 0x33, 0x3B));

        p.save();
        p.translate(0, TOP_PAD);
        drawPlayer(p);
        drawArt(p);
        drawTabs(p);
        drawVideo(p);
        p.restore();

        // playlistArea() retains its logical -TOP_PAD origin so existing
        // hit-testing stays aligned; compensate locally when painting it.
        p.save();
        p.translate(0, TOP_PAD);
        drawPlaylist(p);
        p.restore();
    }

    /* ---- interaction -------------------------------------------------------- */

    QPoint BentoWidget::logical(QMouseEvent * e) const
    {
        #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return e->position().toPoint() - QPoint(0, TOP_PAD);
        #else
        return e->pos() - QPoint(0, TOP_PAD);
        #endif
    }

    /* Map widget coords into the player skin space.  When the left column is
     * narrower than the original skin the player is drawn scaled; hit-tests
     * must undo that scale.  When wider, right-side controls are shifted. */
    QPoint BentoWidget::toSkin(const QPoint & pt) const
    {
        const int pw = playerWidth();
        if (pw < PLAYER_W && pw > 0)
            return QPoint(int(pt.x() * double(PLAYER_W) / pw + 0.5), pt.y());
        return pt;
    }

    int BentoWidget::buttonAt(const QPoint & pt) const
    {
        if (!playerArea().contains(pt))
            return -1;

        const int pw = playerWidth();
        if (pw < PLAYER_W)
        {
            QPoint skin = toSkin(pt);
            for (int id = 0; id < B_COUNT; id++)
                if (kBtnRect[id].contains(skin))
                    return id;
            return -1;
        }

        for (int id = 0; id < B_COUNT; id++)
            if (btnRect(id).contains(pt))
                return id;
        return -1;
    }

    void BentoWidget::setVolumeFromX(int x)
    {
        const int pw = playerWidth();
        QRect volR = (pw < PLAYER_W) ? kVolSlider : volSliderRect();
        int sx = (pw < PLAYER_W && pw > 0) ? int(x * double(PLAYER_W) / pw + 0.5) : x;
        double frac = (sx - volR.x() - kVolThumbW / 2.0) / (volR.width() - kVolThumbW);
        int v = int(qBound(0.0, frac, 1.0) * 100 + 0.5);
        aud_drct_set_volume_main(v);
        m_volume = v;
        m_premute = -1;
        noteVolumeChange();
    }

    void BentoWidget::noteVolumeChange()
    {
        m_volReadoutUntil = m_clock.elapsed() + 1000;
        update();
    }

    void BentoWidget::setSeekFromX(int x)
    {
        const int pw = playerWidth();
        QRect seekR = (pw < PLAYER_W) ? kSeekSlider : seekSliderRect();
        int sx = (pw < PLAYER_W && pw > 0) ? int(x * double(PLAYER_W) / pw + 0.5) : x;
        double frac = (sx - seekR.x() - kSeekThumbW / 2.0) / (seekR.width() - kSeekThumbW);
        m_dragFrac = qBound(0.0, frac, 1.0);
        update();
    }

    void BentoWidget::toggleMute()
    {
        if (m_premute >= 0)
        {
            aud_drct_set_volume_main(m_premute);
            m_premute = -1;
        }
        else
        {
            m_premute = aud_drct_get_volume_main();
            aud_drct_set_volume_main(0);
        }
        m_volume = aud_drct_get_volume_main();
        noteVolumeChange();
    }

    void BentoWidget::openFiles()
    {
        QStringList files = QFileDialog::getOpenFileNames(this, "Open file(s)");
        bool first = true;
        for (const QString & f : files)
        {
            QByteArray uri = QUrl::fromLocalFile(f).toEncoded();
            if (first)
                aud_drct_pl_open(uri.constData());
            else
                aud_drct_pl_add(uri.constData(), -1);
            first = false;
        }
    }

    void BentoWidget::playEntry(int index)
    {
        if (index < 0 || index >= m_entries.size())
            return;
        Playlist pl = Playlist::active_playlist();
        // set_position + start_playback forces a switch even when a song from the
        // previous session is already playing (aud_drct_play() alone would not).
        pl.set_position(index);
        pl.start_playback();
    }

    void BentoWidget::activate(int id)
    {
        switch (id)
        {
            case B_PREV:  aud_drct_pl_prev(); break;
            case B_PLAY:  aud_drct_play(); break;
            case B_PAUSE: aud_drct_pause(); break;
            case B_STOP:  aud_drct_stop(); break;
            case B_NEXT:  aud_drct_pl_next(); break;
            case B_EJECT: openFiles(); break;
            case B_SHUFFLE:
                aud_set_bool(nullptr, "shuffle", !aud_get_bool(nullptr, "shuffle"));
                break;
            case B_REPEAT:
                aud_set_bool(nullptr, "repeat", !aud_get_bool(nullptr, "repeat"));
                break;
            case B_MUTE: toggleMute(); break;
            default: break;
        }
        tick();
    }

    void BentoWidget::mousePressEvent(QMouseEvent * e)
    {
        if (e->button() != Qt::LeftButton)
            return;

        QPoint pt = logical(e);

        // splitter between video and playlist
        if (splitterRect().contains(pt))
        {
            m_drag = Drag::Splitter;
            m_splitDragX = pt.x();
            m_splitDragStartW = m_plW;
            setCursor(Qt::SplitHCursor);
            return;
        }

        // player skin region (stretches / shrinks with the left column)
        if (playerArea().contains(pt))
        {
            int b = buttonAt(pt);
            if (b >= 0)
            {
                m_down = b;
                update();
                return;
            }
            const int pw = playerWidth();
            QPoint skin = toSkin(pt);
            QRect volR = (pw < PLAYER_W) ? kVolSlider : volSliderRect();
            QRect seekR = (pw < PLAYER_W) ? kSeekSlider : seekSliderRect();
            QPoint hit = (pw < PLAYER_W) ? skin : pt;

            if (volR.contains(hit))
            {
                m_drag = Drag::Volume;
                setVolumeFromX(pt.x());
                return;
            }
            if (seekR.contains(hit) && m_playing && m_length > 0)
            {
                m_drag = Drag::Seek;
                setSeekFromX(pt.x());
                return;
            }
            if (kTimeRect.contains(skin))
            {
                m_remaining = !m_remaining;
                update();
                return;
            }
        }

        // tabs — switch active tab (visual only)
        // tabs under player
        QRect tabs = tabArea();
        if (tabs.contains(pt))
        {
            QFontMetrics fm(m_tabFont);
            int x = tabs.x() + 4;
            for (int i = 0; i < kTabCount; i++)
            {
                int tw = fm.horizontalAdvance(kTabNames[i]) + 16;
                if (pt.x() >= x && pt.x() < x + tw)
                {
                    m_activeTab = i;
                    update();
                    return;
                }
                x += tw + 2;
            }
        }

        // wrench → Audacious settings
        if (prefsButtonRect().contains(pt))
        {
            aud_ui_show_prefs_window();
            return;
        }

        // playlist pane
        QRect pl = playlistArea();
        if (pl.contains(pt))
        {
            // playlist-selector tabs
            if (plusButtonRect().contains(pt))
            {
                newPlaylist();
                return;
            }

            int tab = plTabAt(pt);
            if (tab >= 0)
            {
                activatePlaylist(tab);
                m_pressTab = tab;
                m_pressPos = pt;
                return;
            }

            QRect thumb = thumbRect();
            if (thumb.isValid() && thumb.contains(pt))
            {
                m_drag = Drag::Scroll;
                m_scrollDragY = pt.y();
                m_scrollDragStart = m_plScroll;
                return;
            }
            setFocus();

            // click in the scrollbar track (not on the thumb): page up / down
            if (scrollbarRect().contains(pt))
            {
                int vis = playlistRowsVisible();
                int maxScroll = qMax(0, m_entries.size() - vis);
                int dir = (pt.y() < thumbRect().top()) ? -vis : vis;
                m_plScroll = qBound(0, m_plScroll + dir, maxScroll);
                update();
                return;
            }

            const Qt::KeyboardModifiers mods = e->modifiers();
            const bool shift = mods.testFlag(Qt::ShiftModifier);
            const bool ctrl = mods.testFlag(Qt::ControlModifier);

            int entry = entryAtY(pt.y());
            if (entry >= 0)
            {
                m_pressRow = entry;
                m_pressPos = pt;
                m_pressCollapse = false;

                if (shift)
                    selectRange(m_anchor >= 0 ? m_anchor : entry, entry, ctrl);
                else if (ctrl)
                    toggleSelect(entry);
                else if (m_entries[entry].selected)
                    m_pressCollapse = true; // may turn into a drag of the whole selection
                else
                    selectOnly(entry);

                if (!shift)
                    m_anchor = entry;
                m_focus = entry;
                m_plHover = entry;
                update();
            }
            else if (!shift && !ctrl)
            {
                selectNone();

                // Empty space in the list itself (not the tab strip or the footer):
                // start a drag-select band.
                const int footerTop = pl.bottom() - PL_FOOTER + 1;
                if (!plTabBarRect().contains(pt) && pt.y() < footerTop)
                {
                    m_drag = Drag::Rubber;
                    m_rubberX0 = m_rubberX1 = pt.x();
                    m_rubberY0 = m_rubberY1 = contentYAt(pt.y());
                    m_rubberLo = m_rubberHi = -1; // nothing selected yet
                }
                update();
            }
        }
    }

    void BentoWidget::mouseDoubleClickEvent(QMouseEvent * e)
    {
        if (e->button() != Qt::LeftButton)
            return;

        QPoint pt = logical(e);
        if (!playlistArea().contains(pt))
            return;

        int entry = entryAtY(pt.y());
        if (entry >= 0)
            playEntry(entry);
    }

    void BentoWidget::mouseMoveEvent(QMouseEvent * e)
    {
        QPoint pt = logical(e);

        // start dragging a playlist tab once the pointer has moved far enough
        if (e->buttons().testFlag(Qt::LeftButton) && m_pressTab >= 0 &&
            m_drag == Drag::None &&
            (pt - m_pressPos).manhattanLength() >= QApplication::startDragDistance())
        {
            m_drag = Drag::TabReorder;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_drag == Drag::TabReorder)
        {
            m_tabDropSlot = tabSlotAt(pt.x());
            update();
            return;
        }

        if (m_drag == Drag::Rubber)
        {
            updateRubber(pt);
            return;
        }

        // start dragging selected playlist rows once the pointer has moved far enough
        if (e->buttons().testFlag(Qt::LeftButton) && m_pressRow >= 0 &&
            m_drag == Drag::None &&
            (pt - m_pressPos).manhattanLength() >= QApplication::startDragDistance())
        {
            m_drag = Drag::PlReorder;
            m_pressCollapse = false;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_drag == Drag::PlReorder)
        {
            m_dropRow = dropRowAt(pt.y());
            update();
            return;
        }

        if (m_drag == Drag::Volume)
            setVolumeFromX(pt.x());
        else if (m_drag == Drag::Seek)
            setSeekFromX(pt.x());
        else if (m_drag == Drag::Scroll)
        {
            int n = m_entries.size();
            int vis = playlistRowsVisible();
            int maxScroll = qMax(0, n - vis);
            if (maxScroll > 0)
            {
                QRect track = thumbTrackRect();
                int dy = pt.y() - m_scrollDragY;
                int range = track.height() - qMax(20, track.height() * vis / n);
                if (range > 0)
                {
                    int delta = dy * maxScroll / range;
                    m_plScroll = qBound(0, m_scrollDragStart + delta, maxScroll);
                    update();
                }
            }
        }
        else if (m_drag == Drag::Splitter)
        {
            int dx = m_splitDragX - pt.x(); // drag left => wider playlist
            m_plW = qBound(PL_MIN_W, m_splitDragStartW + dx, maxPlaylistWidth());
            m_leftW = m_w - m_plW;
            update();
        }

        // cursor over splitter
        if (m_drag == Drag::None)
        {
            if (splitterRect().contains(pt))
                setCursor(Qt::SplitHCursor);
            else
                unsetCursor();
        }

        int h = buttonAt(pt);
        if (h != m_hover)
        {
            m_hover = h;
            update();
        }

        int pe = -1;
        int th = -1;
        const bool plusH = playlistArea().contains(pt) && plusButtonRect().contains(pt);
        if (playlistArea().contains(pt))
        {
            th = plTabAt(pt);
            if (th < 0)
                pe = entryAtY(pt.y());
        }
        if (pe != m_plHover || th != m_plTabHover || plusH != m_plusHover)
        {
            m_plHover = pe;
            m_plTabHover = th;
            m_plusHover = plusH;
            update();
        }
    }

    void BentoWidget::mouseReleaseEvent(QMouseEvent * e)
    {
        if (e->button() != Qt::LeftButton)
            return;

        QPoint pt = logical(e);

        if (m_drag == Drag::TabReorder)
        {
            const int from = m_pressTab;
            const int b = m_tabDropSlot;
            const int to = (b > from) ? b - 1 : b;
            const bool valid = from >= 0 && from < m_plTitles.size() && b >= 0;
            cancelTabDrag();
            if (valid && to != from)
            {
                shimReorderPlaylist(from, to);
                refreshPlaylistTabs(); // moved playlist stays active; strip scrolls to it
            }
            update();
            return;
        }
        m_pressTab = -1;

        if (m_drag == Drag::Rubber)
        {
            m_drag = Drag::None;
            update();
            return;
        }

        if (m_drag == Drag::PlReorder)
        {
            const int anchor = m_pressRow;
            const int drop = m_dropRow;
            m_drag = Drag::None;
            m_pressRow = -1;
            m_dropRow = -1;
            unsetCursor();
            if (anchor >= 0 && drop >= 0)
                moveSelected(anchor, drop);
            update();
            return;
        }
        if (m_pressRow >= 0)
        {
            // plain click on a row of a multi-selection: collapse to that row
            if (m_pressCollapse && entryAtY(pt.y()) == m_pressRow)
            {
                selectOnly(m_pressRow);
                update();
            }
            m_pressRow = -1;
            m_pressCollapse = false;
        }

        if (m_down >= 0)
        {
            int id = m_down;
            m_down = -1;
            update();
            if (buttonAt(pt) == id)
                activate(id);
        }

        if (m_drag == Drag::Seek)
        {
            if (m_length > 0)
            {
                m_seekTarget = int(m_dragFrac * m_length);
                m_seekHoldUntil = m_clock.elapsed() + 1500;
                m_time = m_seekTarget;
                aud_drct_seek(m_seekTarget);
            }
            m_drag = Drag::None;
            update();
        }
        else if (m_drag == Drag::Volume || m_drag == Drag::Scroll ||
            m_drag == Drag::Splitter)
        {
            m_drag = Drag::None;
            unsetCursor();
            update();
        }
    }

    /* Middle button held + wheel: seek 5 seconds per notch, scroll up = forward,
     * scroll down = backward.  Delta is accumulated so smooth-scrolling wheels and
     * touchpads (which send many small deltas) still move 5 s per full notch. */
    void BentoWidget::seekByWheel(int dy)
    {
        m_seekWheelAccum += dy;
        const int steps = m_seekWheelAccum / 120;
        if (steps == 0)
            return;
        m_seekWheelAccum -= steps * 120;

        if (!aud_drct_get_playing() || !aud_drct_get_ready())
            return;
        const int length = aud_drct_get_length();
        if (length <= 0) // streams of unknown length can't be seeked this way
            return;

        const int target = qBound(0, aud_drct_get_time() + steps * 5000, length - 1);
        aud_drct_seek(target);
        m_time = target;
        update();
    }

    void BentoWidget::wheelEvent(QWheelEvent * e)
    {
        if (e->buttons() & Qt::MiddleButton)
        {
            seekByWheel(e->angleDelta().y());
            e->accept();
            return;
        }
        m_seekWheelAccum = 0;

        #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPoint wpt = e->position().toPoint() - QPoint(0, TOP_PAD);
        #else
        QPoint wpt = e->pos() - QPoint(0, TOP_PAD);
        #endif

        // Tab strip: scroll the tabs sideways
        if (plTabBarRect().contains(wpt))
        {
            const int maxScroll = qMax(0, tabsTotalWidth() - tabsAvailWidth());
            if (maxScroll <= 0)
            {
                e->ignore();
                return;
            }
            const int delta = e->angleDelta().y() > 0 ? -40 : 40;
            m_plTabScroll = qBound(0, m_plTabScroll + delta, maxScroll);
            update();
            e->accept();
            return;
        }

        // Ctrl+wheel over the playlist: change its font size by 1px per notch
        if ((e->modifiers() & Qt::ControlModifier) && playlistArea().contains(wpt))
        {
            const int dy = e->angleDelta().y();
            const int px = qBound(8, m_plFontPx + (dy > 0 ? 1 : dy < 0 ? -1 : 0), 28);
            if (px != m_plFontPx)
            {
                m_plFontPx = px;
                m_plFont.setPixelSize(px);
                m_plScroll = qMax(0, qMin(m_plScroll, m_entries.size() - playlistRowsVisible()));
                update();
            }
            e->accept();
            return;
        }

        // Playlist area: scroll the list
        if (playlistArea().contains(wpt))
        {
            int n = m_entries.size();
            int vis = playlistRowsVisible();
            int maxScroll = qMax(0, n - vis);
            if (maxScroll <= 0)
            {
                e->ignore();
                return;
            }
            int delta = e->angleDelta().y() > 0 ? -3 : 3;
            m_plScroll = qBound(0, m_plScroll + delta, maxScroll);
            update();
            e->accept();
            return;
        }

        // Player or video area: mouse wheel adjusts volume
        if (QRect(0, 0, leftColumnWidth(), PLAYER_H).contains(wpt) || videoArea().contains(wpt) ||
            tabArea().contains(wpt))
        {
            int vol = aud_drct_get_volume_main();
            // One notch ≈ 2% — same direction as typical media players
            int step = e->angleDelta().y() > 0 ? 2 : -2;
            // angleDelta can be larger for high-res wheels
            int notches = e->angleDelta().y() / 120;
            if (notches != 0)
                step = notches * 2;
            vol = qBound(0, vol + step, 100);
            aud_drct_set_volume_main(vol);
            if (m_premute >= 0 && vol > 0)
                m_premute = -1;
            m_volume = vol;
            noteVolumeChange();
            e->accept();
            return;
        }

        e->ignore();
    }

    void BentoWidget::leaveEvent(QEvent *)
    {
        if (m_hover != -1 || m_plHover != -1 || m_plTabHover != -1 || m_plusHover)
        {
            m_hover = -1;
            m_plHover = -1;
            m_plTabHover = -1;
            m_plusHover = false;
            update();
        }
        if (m_drag == Drag::None)
            unsetCursor();
    }

    /* ---- playlist selection & editing ----------------------------------------
     * Selection lives in the real Audacious playlist (select_entry etc.), so
     * "Remove", "Song Info" and friends elsewhere in Audacious see the same
     * rows.  m_entries[i].selected is a mirror that is updated immediately so
     * the UI responds before the "playlist update" hook arrives. */

    int BentoWidget::selectedCount() const
    {
        int c = 0;
        for (const PlEntry & e : m_entries)
            if (e.selected)
                c++;
        return c;
    }

    void BentoWidget::selectOnly(int row)
    {
        Playlist pl = Playlist::active_playlist();
        pl.select_all(false);
        if (row >= 0 && row < m_entries.size())
            pl.select_entry(row, true);
        for (int i = 0; i < m_entries.size(); i++)
            m_entries[i].selected = (i == row);
    }

    void BentoWidget::selectNone()
    {
        Playlist::active_playlist().select_all(false);
        for (PlEntry & e : m_entries)
            e.selected = false;
    }

    void BentoWidget::selectAll()
    {
        Playlist::active_playlist().select_all(true);
        for (PlEntry & e : m_entries)
            e.selected = true;
    }

    void BentoWidget::selectRange(int a, int b, bool additive)
    {
        const int n = m_entries.size();
        if (n == 0)
            return;
        const int lo = qBound(0, qMin(a, b), n - 1);
        const int hi = qBound(0, qMax(a, b), n - 1);

        Playlist pl = Playlist::active_playlist();
        if (!additive)
        {
            pl.select_all(false);
            for (PlEntry & e : m_entries)
                e.selected = false;
        }
        for (int i = lo; i <= hi; i++)
        {
            pl.select_entry(i, true);
            m_entries[i].selected = true;
        }
    }

    void BentoWidget::toggleSelect(int row)
    {
        if (row < 0 || row >= m_entries.size())
            return;
        const bool s = !m_entries[row].selected;
        Playlist::active_playlist().select_entry(row, s);
        m_entries[row].selected = s;
    }

    void BentoWidget::scrollToRow(int row)
    {
        const int vis = playlistRowsVisible();
        if (row < m_plScroll)
            m_plScroll = row;
        else if (row >= m_plScroll + vis)
            m_plScroll = row - vis + 1;
        m_plScroll = qMax(0, m_plScroll);
    }

    void BentoWidget::moveFocus(int delta, bool extend)
    {
        const int n = m_entries.size();
        if (n == 0)
            return;

        const int base = (m_focus >= 0) ? m_focus : (delta > 0 ? -1 : n);
        const int target = qBound(0, base + delta, n - 1);

        if (extend)
        {
            if (m_anchor < 0)
                m_anchor = (m_focus >= 0) ? m_focus : target;
            selectRange(m_anchor, target, false);
        }
        else
        {
            selectOnly(target);
            m_anchor = target;
        }

        m_focus = target;
        scrollToRow(target);
        update();
    }

    int BentoWidget::dropRowAt(int y) const
    {
        const int n = m_entries.size();
        const int listTop = this->listTop();
        const int first = firstRow();
        // round to the nearest row boundary
        const int rel = y - listTop - firstRowOffset() + plRow() / 2;
        int row = first + (rel >= 0 ? rel / plRow() : -1);
        const int maxRow = qMin(n, first + playlistRowsVisible() + (pinnedToBottom() ? 1 : 0));
        return qBound(0, row, maxRow);
    }

    /* Drag-to-sort.  `dropRow` is a boundary: "insert before row dropRow". */
    void BentoWidget::moveSelected(int anchorRow, int dropRow)
    {
        const int n = m_entries.size();
        if (anchorRow < 0 || anchorRow >= n || dropRow < 0 || dropRow > n ||
            !m_entries[anchorRow].selected)
            return;

        int first = n, last = -1, count = 0;
        for (int i = 0; i < n; i++)
            if (m_entries[i].selected)
            {
                first = qMin(first, i);
                last = i;
                count++;
            }

        // dropping on or inside a contiguous block changes nothing
        if (last - first + 1 == count && dropRow >= first && dropRow <= last + 1)
            return;

        int distance;
        if (dropRow > last)
            distance = dropRow - 1 - last;  // block lands just above the boundary
        else if (dropRow <= first)
            distance = dropRow - first;     // block lands starting at the boundary
        else                                // inside a scattered selection
            distance = (dropRow > anchorRow) ? dropRow - 1 - anchorRow
                                             : dropRow - anchorRow;
        if (distance == 0)
            return;

        shimShift(Playlist::active_playlist(), anchorRow, distance);

        if (m_focus >= 0)
            m_focus = qBound(0, m_focus + distance, n - 1);
        if (m_anchor >= 0)
            m_anchor = qBound(0, m_anchor + distance, n - 1);
    }

    void BentoWidget::removeSelected()
    {
        int first = -1;
        for (int i = 0; i < m_entries.size(); i++)
            if (m_entries[i].selected)
            {
                first = i;
                break;
            }
        if (first < 0)
            return;

        Playlist::active_playlist().remove_selected();
        m_focus = first; // clamped when the playlist refreshes
        m_anchor = first;
    }

    void BentoWidget::copySelected(bool cut)
    {
        Playlist pl = Playlist::active_playlist();
        QList<QUrl> urls;
        for (int i = 0; i < m_entries.size(); i++)
        {
            if (!m_entries[i].selected)
                continue;
            String fn = pl.entry_filename(i);
            const char * c = fn;
            if (c)
                urls.append(QUrl::fromEncoded(QByteArray(c)));
        }
        if (urls.isEmpty())
            return;

        QMimeData * md = new QMimeData;
        md->setUrls(urls);
        QApplication::clipboard()->setMimeData(md); // clipboard takes ownership

        if (cut)
            removeSelected();
    }

    void BentoWidget::pasteAtFocus()
    {
        const QMimeData * md = QApplication::clipboard()->mimeData();
        if (!md || !md->hasUrls())
            return;
        insertUrls(Playlist::active_playlist(), m_focus >= 0 ? m_focus + 1 : -1,
                   md->urls());
    }

    void BentoWidget::queueToggleSelected()
    {
        Playlist pl = Playlist::active_playlist();
        for (int i = 0; i < m_entries.size(); i++)
            if (m_entries[i].selected)
                shimQueueToggle(pl, i);
        update();
    }

    void BentoWidget::songInfoFocus()
    {
        int row = m_focus;
        if (row < 0 || row >= m_entries.size())
        {
            for (int i = 0; i < m_entries.size(); i++)
                if (m_entries[i].selected)
                {
                    row = i;
                    break;
                }
        }
        if (row >= 0 && row < m_entries.size())
            shimSongInfo(Playlist::active_playlist(), row);
    }

    int BentoWidget::contentYAt(int y) const
    {
        return y - listTop() - firstRowOffset() + firstRow() * plRow();
    }

    /* Drag-select: select every row the band touches.  A band that only covers the
     * empty space below the last entry selects nothing. */
    void BentoWidget::updateRubber(const QPoint & pt)
    {
        const int n = m_entries.size();
        const int rowH = plRow();
        m_rubberX1 = pt.x();
        m_rubberY1 = contentYAt(pt.y());

        const int top = qMin(m_rubberY0, m_rubberY1);
        const int bot = qMax(m_rubberY0, m_rubberY1);

        int lo = -1, hi = -1;
        if (n > 0 && bot >= 0 && top < n * rowH)
        {
            lo = qBound(0, top / rowH, n - 1);
            hi = qBound(0, bot / rowH, n - 1);
        }

        if (lo != m_rubberLo || hi != m_rubberHi) // only touch Audacious when it changes
        {
            m_rubberLo = lo;
            m_rubberHi = hi;
            if (lo < 0)
                selectNone();
            else
            {
                selectRange(lo, hi, false);
                m_anchor = lo;
                m_focus = hi;
            }
        }
        update();
    }

    /* While dragging rows (or files from outside) near the top / bottom edge of
     * the list, keep scrolling so far-away positions can be reached. */
    bool BentoWidget::autoScroll()
    {
        if (m_drag == Drag::TabReorder)
        {
            const QPoint cur = mapFromGlobal(QCursor::pos()) - QPoint(0, TOP_PAD);
            const int lo = playlistArea().x() + PL_TAB_X0;
            const int hi = plusButtonRect().left();
            int step = 0;
            if (cur.x() < lo + 16)
                step = -6;
            else if (cur.x() > hi - 16)
                step = 6;
            if (step == 0)
                return false;

            const int maxScroll = qMax(0, tabsTotalWidth() - tabsAvailWidth());
            const int next = qBound(0, m_plTabScroll + step, maxScroll);
            if (next == m_plTabScroll)
                return false;
            m_plTabScroll = next;
            m_tabDropSlot = tabSlotAt(cur.x());
            return true;
        }

        if (m_drag != Drag::PlReorder && !m_extDrag)
            return false;

        const QPoint pt = mapFromGlobal(QCursor::pos()) - QPoint(0, TOP_PAD);
        const QRect area = playlistArea();
        if (m_extDrag && (pt.x() < area.left() || pt.x() > area.right()))
            return false;

        const int listTop = this->listTop();
        const int listBot = m_h - PL_FOOTER - PL_BOTTOM;
        int step = 0;
        if (pt.y() < listTop + plRow() / 2)
            step = -1;
        else if (pt.y() > listBot - plRow() / 2)
            step = 1;
        if (step == 0)
            return false;

        const int maxScroll = qMax(0, m_entries.size() - playlistRowsVisible());
        const int next = qBound(0, m_plScroll + step, maxScroll);
        if (next == m_plScroll)
            return false;

        m_plScroll = next;
        if (m_drag == Drag::PlReorder || m_dropRow >= 0)
            m_dropRow = dropRowAt(pt.y());
        return true;
    }

    /* ---- keyboard ------------------------------------------------------------ */

    void BentoWidget::keyPressEvent(QKeyEvent * e)
    {
        const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);
        const bool alt = e->modifiers().testFlag(Qt::AltModifier);
        const int page = qMax(1, playlistRowsVisible() - 1);
        const int n = m_entries.size();

        if (e->matches(QKeySequence::SelectAll))
        {
            selectAll();
            update();
        }
        else if (e->matches(QKeySequence::Copy))
            copySelected(false);
        else if (e->matches(QKeySequence::Cut))
            copySelected(true);
        else if (e->matches(QKeySequence::Paste))
            pasteAtFocus();
        else if (alt && e->key() == Qt::Key_Q)
            queueToggleSelected();
        else if (alt && e->key() == Qt::Key_I)
            songInfoFocus();
        else
        {
            switch (e->key())
            {
                case Qt::Key_Up:       moveFocus(-1, shift); break;
                case Qt::Key_Down:     moveFocus(1, shift); break;
                case Qt::Key_PageUp:   moveFocus(-page, shift); break;
                case Qt::Key_PageDown: moveFocus(page, shift); break;
                case Qt::Key_Home:     moveFocus(-n, shift); break;
                case Qt::Key_End:      moveFocus(n, shift); break;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    if (m_focus >= 0)
                        playEntry(m_focus);
                    break;
                case Qt::Key_Delete:
                    removeSelected();
                    break;
                default:
                    QWidget::keyPressEvent(e);
                    return;
            }
        }
        e->accept();
    }

    /* ---- playlist tabs ------------------------------------------------------- */

    void BentoWidget::newPlaylist()
    {
        Playlist pl = shimNewPlaylist();
        pl.activate();
        m_plScroll = 0;
        m_focus = m_anchor = -1;
        refreshPlaylist(); // also scrolls the tab strip to the new tab
        update();
    }

    void BentoWidget::renamePlaylist(int tab)
    {
        if (tab < 0 || tab >= Playlist::n_playlists())
            return;

        Playlist pl = Playlist::by_index(tab);
        bool ok = false;
        const QString text = QInputDialog::getText(this, "Rename Playlist", "Playlist name:",
                                                   QLineEdit::Normal, qs(pl.get_title()), &ok);
        if (ok && !text.trimmed().isEmpty())
        {
            shimSetTitle(pl, text.trimmed());
            refreshPlaylistTabs();
            update();
        }
    }

    void BentoWidget::removePlaylist(int tab)
    {
        if (tab < 0 || tab >= Playlist::n_playlists())
            return;

        Playlist pl = Playlist::by_index(tab);
        const int count = pl.n_entries();
        if (count > 0)
        {
            const QString msg = QString("Remove playlist \"%1\"?\n\nIt contains %2 %3.")
                                    .arg(qs(pl.get_title()))
                                    .arg(count)
                                    .arg(count == 1 ? "entry" : "entries");
            if (QMessageBox::question(this, "Remove Playlist", msg,
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No) != QMessageBox::Yes)
                return;
        }

        shimRemovePlaylist(pl);
        m_plScroll = 0;
        m_focus = m_anchor = -1;
        refreshPlaylist();
        update();
    }

    void BentoWidget::showTabMenu(int tab, const QPoint & globalPos)
    {
        QMenu menu(this);
        QAction * aPlay = menu.addAction(QIcon::fromTheme("media-playback-start"), "Play");
        QAction * aRename = menu.addAction(QIcon::fromTheme("document-edit"), "Rename...");
        QAction * aImport = menu.addAction(QIcon::fromTheme("document-open"), "Import...");
        QAction * aExport = menu.addAction(QIcon::fromTheme("document-save-as"), "Export...");
        menu.addSeparator();
        QAction * aRemove = menu.addAction(QIcon::fromTheme("edit-delete"), "Remove");

        aPlay->setEnabled(Playlist::by_index(tab).n_entries() > 0);

        QAction * chosen = menu.exec(globalPos);
        if (!chosen)
            return;

        if (chosen == aPlay)
        {
            Playlist pl = Playlist::by_index(tab);
            pl.activate();
            pl.start_playback();
            refreshPlaylist();
            update();
        }
        else if (chosen == aRename)
            renamePlaylist(tab);
        else if (chosen == aImport)
        {
            // Audacious imports into the active playlist, so make this tab the active one
            Playlist::by_index(tab).activate();
            refreshPlaylist();
            update();
            shimImportPlaylist();
        }
        else if (chosen == aExport)
        {
            // Audacious exports the active playlist, so make this tab the active one
            Playlist::by_index(tab).activate();
            refreshPlaylist();
            update();
            shimExportPlaylist();
        }
        else if (chosen == aRemove)
            removePlaylist(tab);
    }

    /* ---- right-click menu ----------------------------------------------------- */

    void BentoWidget::contextMenuEvent(QContextMenuEvent * e)
    {
        const QPoint pt = e->pos() - QPoint(0, TOP_PAD);
        if (!playlistArea().contains(pt))
        {
            e->ignore();
            return;
        }

        // right-click on a playlist tab: tab menu
        const int tab = plTabAt(pt);
        if (tab >= 0)
        {
            showTabMenu(tab, e->globalPos());
            e->accept();
            return;
        }
        if (plTabBarRect().contains(pt)) // empty part of the tab strip / the "+"
        {
            e->ignore();
            return;
        }

        // like the stock playlist: right-clicking an unselected row selects it
        const int row = entryAtY(pt.y());
        if (row >= 0)
        {
            if (!m_entries[row].selected)
            {
                selectOnly(row);
                m_anchor = row;
            }
            m_focus = row;
        }
        update();

        const int nSel = selectedCount();
        const QMimeData * clip = QApplication::clipboard()->mimeData();
        const bool canPaste = clip && clip->hasUrls();

        QMenu menu(this);
        QAction * aPlay = menu.addAction(QIcon::fromTheme("media-playback-start"), "Play");
        QAction * aInfo = menu.addAction(QIcon::fromTheme("dialog-information"), "Song Info...");
        QAction * aFolder = menu.addAction(QIcon::fromTheme("folder-open"), "Open Containing Folder");
        QAction * aQueue = menu.addAction("Queue/Unqueue");
        menu.addSeparator();
        QAction * aCut = menu.addAction(QIcon::fromTheme("edit-cut"), "Cut");
        QAction * aCopy = menu.addAction(QIcon::fromTheme("edit-copy"), "Copy");
        QAction * aPaste = menu.addAction(QIcon::fromTheme("edit-paste"), "Paste");
        QAction * aAll = menu.addAction(QIcon::fromTheme("edit-select-all"), "Select All");
        menu.addSeparator();
        QAction * aRemove = menu.addAction(QIcon::fromTheme("edit-delete"), "Remove");

        aPlay->setShortcut(QKeySequence(Qt::Key_Return));
        aInfo->setShortcut(QKeySequence("Alt+I"));
        aQueue->setShortcut(QKeySequence("Alt+Q"));
        aCut->setShortcut(QKeySequence::Cut);
        aCopy->setShortcut(QKeySequence::Copy);
        aPaste->setShortcut(QKeySequence::Paste);
        aAll->setShortcut(QKeySequence::SelectAll);
        aRemove->setShortcut(QKeySequence::Delete);

        aPlay->setEnabled(m_focus >= 0);
        aInfo->setEnabled(nSel > 0 || m_focus >= 0);
        const QString focusUri = (m_focus >= 0)
            ? qs(Playlist::active_playlist().entry_filename(m_focus)) : QString();
        aFolder->setEnabled(focusUri.startsWith("file://"));
        aQueue->setEnabled(nSel > 0);
        aCut->setEnabled(nSel > 0);
        aCopy->setEnabled(nSel > 0);
        aPaste->setEnabled(canPaste);
        aAll->setEnabled(!m_entries.isEmpty());
        aRemove->setEnabled(nSel > 0);

        QAction * chosen = menu.exec(e->globalPos());
        e->accept();
        if (!chosen)
            return;

        if (chosen == aPlay)
            playEntry(m_focus);
        else if (chosen == aInfo)
            songInfoFocus();
        else if (chosen == aFolder)
        {
            const QString folder = focusUri.left(focusUri.lastIndexOf('/') + 1);
            if (!QDesktopServices::openUrl(QUrl(folder)))
                AUDERR("Failed to open folder %s\n", folder.toUtf8().constData());
        }
        else if (chosen == aQueue)
            queueToggleSelected();
        else if (chosen == aCut)
            copySelected(true);
        else if (chosen == aCopy)
            copySelected(false);
        else if (chosen == aPaste)
            pasteAtFocus();
        else if (chosen == aAll)
        {
            selectAll();
            update();
        }
        else if (chosen == aRemove)
            removeSelected();
    }

    /* ---- files dragged in from outside --------------------------------------- */

    void BentoWidget::dragEnterEvent(QDragEnterEvent * e)
    {
        if (e->mimeData()->hasUrls())
        {
            m_extDrag = true;
            e->acceptProposedAction();
        }
        else
            e->ignore();
    }

    void BentoWidget::dragMoveEvent(QDragMoveEvent * e)
    {
        if (!e->mimeData()->hasUrls())
        {
            e->ignore();
            return;
        }

        const QPoint pt = dropPoint(e);
        const bool inList = playlistArea().contains(pt);
        const int tab = inList ? plTabAt(pt) : -1;
        const int drop = (inList && tab < 0) ? dropRowAt(pt.y()) : -1;

        if (drop != m_dropRow || tab != m_plTabHover)
        {
            m_dropRow = drop;
            m_plTabHover = tab;
            update();
        }
        e->acceptProposedAction();
    }

    void BentoWidget::dragLeaveEvent(QDragLeaveEvent * e)
    {
        m_extDrag = false;
        m_dropRow = -1;
        m_plTabHover = -1;
        update();
        e->accept();
    }

    void BentoWidget::dropEvent(QDropEvent * e)
    {
        m_extDrag = false;

        const QMimeData * md = e->mimeData();
        if (!md->hasUrls())
        {
            e->ignore();
            return;
        }

        const QPoint pt = dropPoint(e);
        const bool inList = playlistArea().contains(pt);
        const int tab = inList ? plTabAt(pt) : -1;

        if (tab >= 0)
            insertUrls(Playlist::by_index(tab), -1, md->urls()); // append to that playlist
        else
            insertUrls(Playlist::active_playlist(),
                       inList ? dropRowAt(pt.y()) : -1, md->urls());

        m_dropRow = -1;
        m_plTabHover = -1;
        e->acceptProposedAction();
        update();
    }

    void BentoWidget::closeEvent(QCloseEvent * e)
    {
        // Closing the main window exits Audacious (we are the interface).
        // Accept first so Qt tears the window down cleanly; aud_quit() asks
        // the interface plugin to leave the event loop.
        e->accept();
        hide();
        aud_quit();
    }

    QPointer<BentoWidget> g_widget;

    static const char * const kHooks[] = {
        "playback begin", "playback ready", "playback stop", "tuple change",
        "playlist update", "playlist activate"};

    static const char * const kArtHooks[] = {"art ready"};

        void hooks_install()
        {
            for (const char * h : kHooks)
                hook_associate(h, meta_hook, nullptr);
            for (const char * h : kArtHooks)
                hook_associate(h, art_hook, nullptr);
        }

        void hooks_remove()
        {
            for (const char * h : kHooks)
                hook_dissociate(h, meta_hook, nullptr);
            for (const char * h : kArtHooks)
                hook_dissociate(h, art_hook, nullptr);
        }

        BentoWidget * create_main_window()
        {
            if (g_widget)
                return g_widget.data();

            if (!QApplication::instance())
                return nullptr;

            g_widget = new BentoWidget;
            return g_widget.data();
        }

        void destroy_main_window()
        {
            delete g_widget;
            g_widget = nullptr;
        }

        BentoWidget * main_window() { return g_widget.data(); }

        void release_main_window()
        {
            // Window is being deleteLater()'d by quit(); just drop the pointer.
            g_widget.clear();
        }

} // namespace

/* ---- plugin boilerplate (full interface plugin) -------------------------
 *
 * Mirrors qtui / skins-qt: derive from audqt::QtIfacePlugin, call
 * audqt::init() before creating any QWidget, audqt::cleanup() on the way
 * out.  That keeps the shared QApplication and audqt state consistent when
 * switching interfaces at runtime.
 */

class BentoIface : public audqt::QtIfacePlugin
{
public:
    static const char about[];

    static constexpr PluginInfo info = {
        "Bento Interface",
        "audacious-bento",
        about,
        nullptr,
        PluginQtOnly,
    };

    constexpr BentoIface() : audqt::QtIfacePlugin(info) {}

    bool init() override;
    void cleanup() override;
    void run() override;
    void show(bool show) override;
    void quit() override;
};

const char BentoIface::about[] =
"Bento Interface for Audacious — Winamp 5 Bento-style main window\n"
"(player, video panel, playlist).\n"
"Skin artwork by Taber Buhl and Martin Poehlmann (Bento, Winamp).\n"
"Skin assets are embedded; no external Bento folder required.\n"
"Select this interface under Settings → Appearance → Interface.";

bool BentoIface::init()
{
    // MUST run before any QWidget — same as qtui / skins-qt
    audqt::init();

    hooks_install();

    // Defer window creation to show() so we are not constructing widgets
    // on the same call stack that just destroyed the previous interface.
    return true;
}

void BentoIface::cleanup()
{
    hooks_remove();
    destroy_main_window();
    audqt::cleanup();
}

void BentoIface::run()
{
    QApplication::exec();
}

void BentoIface::show(bool show)
{
    if (show && !main_window())
        create_main_window();

    BentoWidget * w = main_window();
    if (!w)
        return;

    w->setVisible(show);
    if (show)
    {
        w->activateWindow();
        w->raise();
    }
}

void BentoIface::quit()
{
    BentoWidget * w = main_window();
    if (w)
    {
        QObject::connect(w, &QObject::destroyed, QApplication::quit);
        release_main_window();
        w->deleteLater();
    }
    else if (QApplication::instance())
        QApplication::quit();
}

EXPORT BentoIface aud_plugin_instance;
