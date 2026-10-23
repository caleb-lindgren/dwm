/* In-process test harness for the custom dwm. Includes dwm.c, drives it
 * against an Xvfb server through a second client connection, and checks
 * targeted regressions plus invariants under a random operation fuzz.
 * Run with `make -C test`; see test/Makefile. */
#include <X11/Xlib.h>
#include <stdio.h>

static unsigned long nprop; /* XGetWindowProperty calls made by dwm */
static int
counting_XGetWindowProperty(Display *d, Window w, Atom p, long o, long l,
	Bool del, Atom req, Atom *at, int *af, unsigned long *ni,
	unsigned long *ba, unsigned char **pr)
{
	nprop++;
	return XGetWindowProperty(d, w, p, o, l, del, req, at, af, ni, ba, pr);
}
#define XGetWindowProperty counting_XGetWindowProperty
#define main dwm_main
#include "dwm.c"
#undef main
#undef XGetWindowProperty

static Display *cdpy;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static void
pump(void)
{
	XEvent ev;
	int k;

	for (k = 0; k < 3; k++) {
		XSync(cdpy, False);
		XSync(dpy, False);
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (handler[ev.type])
				handler[ev.type](&ev);
		}
	}
}

static Window
mkwin(const char *name)
{
	Window w = XCreateSimpleWindow(cdpy, DefaultRootWindow(cdpy),
		0, 0, 100, 100, 0, 0, 0);
	XStoreName(cdpy, w, name);
	XMapWindow(cdpy, w);
	pump();
	return w;
}

static int
truehidden(Client *c)
{
	return getstate(c->win) == IconicState;
}

static int
mapstate(Window w)
{
	XWindowAttributes wa;
	if (!XGetWindowAttributes(cdpy, w, &wa))
		return -1;
	return wa.map_state;
}

static Window
xfocus(void)
{
	Window w;
	int rev;
	XGetInputFocus(dpy, &w, &rev);
	return w;
}

static void
destroyall(void)
{
	Monitor *m;
	for (m = mons; m; m = m->next)
		while (m->clients) {
			XDestroyWindow(cdpy, m->clients->win);
			pump();
		}
}

static void
viewtag(unsigned int t)
{
	Arg a = {.ui = t};
	view(&a);
	pump();
}

static int
nhidden(void)
{
	Monitor *m;
	Client *c;
	int n = 0;
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			n += truehidden(c);
	return n;
}

/* --- targeted tests --- */

static void
test_toggleview_all(void)
{
	unsigned int i;
	Arg a;

	viewtag(1);
	for (i = 1; i < LENGTH(tags); i++) {
		a.ui = 1 << i;
		toggleview(&a);
	}
	CHECK(selmon->tagset[selmon->seltags] == TAGMASK, "not all tags");
	CHECK(selmon->pertag->curtag == 0,
		"toggling every tag on should select the all-tags pertag slot,"
		" got curtag %u", selmon->pertag->curtag);
	viewtag(~0);
	a.ui = 1; /* toggle tag 1 off while curtag is 0: 1 << -1 */
	toggleview(&a);
	CHECK(selmon->pertag->curtag == 2, "curtag %u, want 2",
		selmon->pertag->curtag);
	viewtag(1);
}

static void
test_view_prev_consistent(void)
{
	Arg a;

	viewtag(1);
	viewtag(1 << 2);
	a.ui = 1 << 3; toggleview(&a);
	a.ui = 1 << 2; toggleview(&a);
	viewtag(0); /* back to the previous tagset, {1} */
	CHECK(selmon->tagset[selmon->seltags] == 1, "tagset %x",
		selmon->tagset[selmon->seltags]);
	CHECK(selmon->pertag->curtag == 1,
		"viewing tag 1 again uses tag %u's layout settings",
		selmon->pertag->curtag);
	viewtag(1);
}

static void
test_focusstackhid_nosel(void)
{
	Window a;
	Client *ca;
	Arg arg = {.i = -1};

	viewtag(1);
	a = mkwin("a");
	ca = wintoclient(a);
	hide(NULL);
	pump();
	CHECK(truehidden(ca), "a not hidden");
	viewtag(2); /* empty tag, no selection */
	focusstackhid(&arg);
	pump();
	CHECK(truehidden(ca), "focusstackhid on an empty tag unhid a client "
		"on another tag");
	viewtag(1);
	destroyall();
}

static void
test_focusstackhid_focus(void)
{
	Window a, h;
	Arg arg = {.i = +1};

	viewtag(1);
	h = mkwin("h");
	hide(NULL);
	pump();
	a = mkwin("a");
	focus(wintoclient(a));
	pump();
	focusstackhid(&arg);
	pump();
	CHECK(selmon->sel == wintoclient(h), "h not selected");
	CHECK(xfocus() == h, "input focus is 0x%lx, not the peeked window "
		"0x%lx", xfocus(), h);
	destroyall();
}

static void
test_peek_then_close(void)
{
	Window a, b, h;
	Arg arg = {.i = +1};

	viewtag(1);
	h = mkwin("h");
	hide(NULL);
	b = mkwin("b");
	a = mkwin("a");
	pump();
	focus(wintoclient(a));
	pump();
	while (selmon->sel != wintoclient(h)) {
		focusstackhid(&arg);
		pump();
	}
	CHECK(selmon->hidsel, "not peeking");
	XDestroyWindow(cdpy, h); /* close the peeked window */
	pump();
	focusstackvis(&arg); /* an ordinary focus change */
	pump();
	focusstackvis(&arg);
	pump();
	CHECK(nhidden() == 0, "%d window(s) hidden after closing a peeked "
		"window and moving focus", nhidden());
	(void)a; (void)b;
	destroyall();
}

static void
test_attachtop_nmaster0(void)
{
	Window w[3];
	Client *c;
	int i, saved = selmon->nmaster;

	viewtag(1);
	for (i = 0; i < 3; i++)
		w[i] = mkwin("t");
	c = wintoclient(w[0]);
	detach(c);
	selmon->nmaster = 0;
	attachtop(c);
	selmon->nmaster = saved;
	CHECK(selmon->clients == c, "nmaster 0: attachtop did not put the "
		"client at the top of the stack area");
	arrange(selmon);
	destroyall();
}

static void
test_title_redraw(void)
{
	Window a, b;
	Client *ca;
	char longname[200];
	int stale, i;

	viewtag(1);
	a = mkwin("a");
	b = mkwin("b");
	focus(wintoclient(b));
	pump();
	ca = wintoclient(a);
	/* long enough to widen a's tab, short enough to fit beside b's */
	for (i = 0; i < 190; i++) {
		longname[i] = 'x';
		longname[i + 1] = '\0';
		if (TEXTW(longname)
		    > (ca->tabw + wintoclient(b)->tabw) / 2 + 20)
			break;
	}
	XStoreName(cdpy, a, longname);
	pump();
	stale = ca->tabw;
	drawbar(selmon);
	CHECK(ca->tabw == stale, "unselected client's tab not redrawn after "
		"its title changed: tab width %d, %d after a redraw", stale,
		ca->tabw);
	destroyall();
}

static int
titlex0(Monitor *m)
{
	unsigned int i;
	int x = 0;
	for (i = 0; i < LENGTH(tags); i++)
		x += TEXTW(tags[i]);
	return x + TEXTW(m->ltsymbol);
}

static void
clickbar(int x, unsigned int button)
{
	XEvent e = {0};
	e.xbutton.type = ButtonPress;
	e.xbutton.window = selmon->barwin;
	e.xbutton.x = x;
	e.xbutton.button = button;
	buttonpress(&e);
	pump();
}

static void
test_title_click(void)
{
	Client *c, *want;
	int i, x, j;
	Arg t = {.ui = 1 << 4};

	viewtag(1);
	for (i = 0; i < 4; i++)
		mkwin(i % 2 ? "short" : "a much longer window title");
	mkwin("other");
	tag(&t);
	pump();
	for (j = 0; j < 4; j++) {
		x = titlex0(selmon);
		want = NULL;
		for (i = 0, c = selmon->clients; c; c = c->next) {
			if (!ISVISIBLE(c))
				continue;
			if (i++ == j) {
				want = c;
				break;
			}
			x += c->tabw;
		}
		if (want == selmon->sel)
			continue;
		clickbar(x + want->tabw / 2, Button1);
		CHECK(selmon->sel == want, "click on tab %d selected %p, "
			"want %p", j, (void *)selmon->sel, (void *)want);
	}
	/* clicking the selected tab hides it */
	c = selmon->sel;
	x = titlex0(selmon);
	for (want = selmon->clients; want != c; want = want->next)
		if (ISVISIBLE(want))
			x += want->tabw;
	clickbar(x + 1, Button1);
	CHECK(truehidden(c), "click on the selected tab did not hide it");
	destroyall();
}

static void
test_pushring(void)
{
	Window w[5];
	Client *order[5], *c;
	Arg dn = {.i = +1}, up = {.i = -1};
	int i, n, k;

	viewtag(1);
	for (i = 0; i < 5; i++)
		w[i] = mkwin("p");
	(void)w;
	for (i = 0, c = nexttiled(selmon->clients); c;
	    c = nexttiled(c->next))
		order[i++] = c;
	n = i;
	focus(order[2]);
	pump();
	for (k = 0; k < n; k++) {
		pushring(&dn);
		pump();
	}
	for (i = 0, c = nexttiled(selmon->clients); c;
	    c = nexttiled(c->next), i++)
		CHECK(c == order[i], "n pushes down is not the identity");
	pushring(&dn);
	pushring(&up);
	pump();
	for (i = 0, c = nexttiled(selmon->clients); c;
	    c = nexttiled(c->next), i++)
		CHECK(c == order[i], "down then up is not the identity");
	focus(order[n - 1]);
	pushring(&dn);
	pump();
	CHECK(nexttiled(selmon->clients) == order[n - 1],
		"last pushed down did not wrap to the first place");
	destroyall();
}

static void
test_settabwidths(void)
{
	Monitor fm = {0};
	Client cl[12], *c;
	int it, n, w, i, total, sum, lo, hi, maxfit;

	fm.tagset[0] = 1;
	srand(1);
	for (it = 0; it < 200000; it++) {
		n = 1 + rand() % 12;
		w = rand() % 2000;
		fm.clients = NULL;
		for (i = n - 1, total = 0; i >= 0; i--) {
			memset(&cl[i], 0, sizeof cl[i]);
			cl[i].mon = &fm;
			cl[i].tags = rand() % 5 ? 1 : 2;
			cl[i].namew = 1 + rand() % 400;
			cl[i].next = fm.clients;
			fm.clients = &cl[i];
		}
		for (n = 0, total = 0, c = fm.clients; c; c = c->next)
			if (ISVISIBLE(c)) {
				n++;
				total += c->namew;
			}
		if (!n)
			continue;
		settabwidths(&fm, n, w);
		/* lo, hi: narrowest and widest truncated tab; maxfit:
		 * widest natural width of a tab that is not truncated */
		lo = 1 << 30; hi = -1; maxfit = 0;
		for (sum = 0, c = fm.clients; c; c = c->next) {
			if (!ISVISIBLE(c))
				continue;
			sum += c->tabw;
			if (total <= w ? c->tabw < c->namew
			    : c->tabw > c->namew)
				break;
			if (c->tabw < c->namew) {
				if (c->tabw < lo) lo = c->tabw;
				if (c->tabw > hi) hi = c->tabw;
			} else if (c->namew > maxfit)
				maxfit = c->namew;
		}
		if (sum != w || c) {
			CHECK(0, "n %d w %d total %d: sum %d", n, w,
				total, sum);
			return;
		}
		if (total > w && (hi - lo > 1 || maxfit > lo + 1)) {
			CHECK(0, "n %d w %d total %d: truncated tabs %d..%d, "
				"untruncated tab up to %d", n, w, total, lo,
				hi, maxfit);
			return;
		}
	}
}

/* put m's per-tag settings back to the config defaults */
static void
resetpertag(Monitor *m)
{
	unsigned int i;

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = nmaster;
		m->pertag->mfacts[i] = mfact;
		m->pertag->sellts[i] = 0;
		m->pertag->ltidxs[i][0] = &layouts[0];
		m->pertag->ltidxs[i][1] = &layouts[1 % LENGTH(layouts)];
		m->pertag->showbars[i] = showbar;
	}
	applypertag(m);
	arrange(m);
}

static const Layout *
slotlayout(Monitor *m, unsigned int slot)
{
	return m->pertag->ltidxs[slot][m->pertag->sellts[slot]];
}

static void
test_switchall_swap(void)
{
	Monitor *src = selmon, *dst = dirtomon(+1);
	Arg mono = {.v = &layouts[2]}, flt = {.v = &layouts[1]};
	Arg inc = {.i = +1}, arg = {.i = +1};
	Client *sorder[4], *dorder[2], *fl, *c;
	int i, ns, nd, x, y;

	/* source views tag 1, monocle with an extra master, plus a
	 * floating window */
	viewtag(1);
	for (i = 0; i < 3; i++)
		mkwin("src");
	setlayout(&mono);
	incnmaster(&inc);
	fl = wintoclient(mkwin("src float"));
	CHECK(fl && fl->mon == src, "floating window not on the source");
	if (!fl)
		return;
	fl->isfloating = 1;
	resize(fl, src->wx + 40, src->wy + 30, 200, 150, 0);
	x = fl->x - src->mx;
	y = fl->y - src->my;
	/* target views tag 2, floating layout */
	selmon = dst;
	viewtag(1 << 1);
	for (i = 0; i < 2; i++)
		mkwin("dst");
	setlayout(&flt);
	selmon = src;
	focus(NULL);
	for (ns = 0, c = src->clients; c; c = c->next)
		sorder[ns++] = c;
	for (nd = 0, c = dst->clients; c; c = c->next)
		dorder[nd++] = c;

	switchall(&arg);
	pump();
	for (i = 0, c = dst->clients; c && i < ns; c = c->next, i++)
		CHECK(c == sorder[i] && c->mon == dst,
			"source client %d not in place on the target", i);
	CHECK(i == ns && !c, "target has the wrong number of clients");
	for (i = 0, c = src->clients; c && i < nd; c = c->next, i++)
		CHECK(c == dorder[i] && c->mon == src,
			"target client %d not in place on the source", i);
	CHECK(i == nd && !c, "source has the wrong number of clients");
	CHECK(dst->tagset[dst->seltags] == 1
		&& src->tagset[src->seltags] == 1 << 1,
		"viewed tags not swapped: target %x, source %x",
		dst->tagset[dst->seltags], src->tagset[src->seltags]);
	CHECK(dst->lt[dst->sellt] == &layouts[2]
		&& dst->nmaster == nmaster + 1,
		"target did not take the source's layout");
	CHECK(src->lt[src->sellt] == &layouts[1],
		"source did not take the target's layout");
	CHECK(fl->x - dst->mx == x && fl->y - dst->my == y,
		"floating window at %d,%d from the target's corner, "
		"want %d,%d", fl->x - dst->mx, fl->y - dst->my, x, y);

	destroyall();
	resetpertag(dst);
	resetpertag(src);
	selmon = dst;
	viewtag(1);
	selmon = src;
	viewtag(1);
	focus(NULL);
	pump();
}

static void
test_removed_monitor_pertag(void)
{
	Monitor *gone, *m;
	Arg mono = {.v = &layouts[2]};
	int n;

	/* a third monitor that the next updategeom() finds missing */
	gone = createmon();
	for (m = mons; m->next; m = m->next);
	m->next = gone;
	gone->num = m->num + 1;
	gone->mx = gone->wx = m->mx;
	gone->my = gone->wy = m->my;
	gone->mw = gone->ww = m->mw;
	gone->mh = gone->wh = m->mh;
	updatebarpos(gone);
	updatebars();

	selmon = gone;
	viewtag(1 << 2);
	mkwin("on the removed monitor");
	CHECK(gone->clients, "window not managed on the extra monitor");
	setlayout(&mono);
	selmon = mons;
	viewtag(1 << 2);

	updategeom();
	arrange(NULL);
	pump();
	for (n = 0, m = mons; m; m = m->next, n++);
	CHECK(n == 2, "%d monitors after the update, want 2", n);
	CHECK(slotlayout(mons, 3) == &layouts[2],
		"tag 3 layout did not move with the removed monitor's "
		"clients");
	CHECK(mons->lt[mons->sellt] == &layouts[2],
		"current view did not take the moved layout");

	destroyall();
	resetpertag(mons);
	selmon = mons;
	viewtag(1);
	focus(NULL);
	pump();
}

static void
test_moved_monitor_floats(void)
{
	Monitor *m = mons;
	Client *c;
	int x, y;

	selmon = m;
	viewtag(1);
	c = wintoclient(mkwin("float"));
	CHECK(c && c->mon == m, "new window not on the first monitor");
	if (!c)
		return;
	c->isfloating = 1;
	resize(c, m->wx + 40, m->wy + 30, 200, 150, 0);
	/* pretend the monitor sat elsewhere until now, as it does after
	 * the primary output changes */
	m->mx -= 100;
	m->my -= 50;
	x = c->x - m->mx;
	y = c->y - m->my;
	updategeom();
	arrange(NULL);
	pump();
	CHECK(c->x - m->mx == x && c->y - m->my == y,
		"floating window at %d,%d from the monitor corner, want %d,%d",
		c->x - m->mx, c->y - m->my, x, y);
	destroyall();
	focus(NULL);
}

static void
test_efficiency(void)
{
	Window w[10];
	unsigned long before;
	int i;
	Arg a;

	viewtag(1);
	for (i = 0; i < 10; i++)
		w[i] = mkwin("e");
	(void)w;
	before = nprop;
	arrange(selmon);
	printf("INFO property reads for one arrange() with 10 clients: %lu\n",
		nprop - before);
	before = nprop;
	drawbar(selmon);
	printf("INFO property reads for one drawbar() with 10 clients: %lu\n",
		nprop - before);
	for (i = 0; i < 10; i++) {
		focus(wintoclient(w[i]));
		hide(NULL);
	}
	pump();
	before = NextRequest(dpy);
	showall(NULL);
	XSync(dpy, False);
	printf("INFO X requests for showall() of 10 hidden clients: %lu\n",
		NextRequest(dpy) - before);
	pump();
	before = NextRequest(dpy);
	a.i = +1;
	switchall(&a);
	XSync(dpy, False);
	printf("INFO X requests for switchall() of 10 clients: %lu\n",
		NextRequest(dpy) - before);
	pump();
	destroyall();
}

/* --- fuzz --- */

#define MAXW 64
static Window fw[MAXW];
static int nfw;

static void
invariants(const char *op, int step)
{
	Monitor *m;
	Client *c, *s;
	int nc, ns, found, i;
	char where[80];

	snprintf(where, sizeof where, "step %d after %s", step, op);
	for (m = mons; m; m = m->next) {
		for (nc = 0, c = m->clients; c && nc < 1000;
		    c = c->next, nc++) {
			CHECK(c->mon == m, "%s: client on wrong monitor",
				where);
			CHECK(c->tags && !(c->tags & ~TAGMASK),
				"%s: bad tags %x", where, c->tags);
			CHECK(HIDDEN(c) == truehidden(c),
				"%s: hidden flag stale", where);
			i = mapstate(c->win);
			CHECK(truehidden(c) ? i == IsUnmapped
				: i == IsViewable,
				"%s: hidden %d but map state %d", where,
				truehidden(c), i);
		}
		for (ns = 0, s = m->stack; s && ns < 1000;
		    s = s->snext, ns++) {
			for (found = 0, c = m->clients; c; c = c->next)
				found |= c == s;
			CHECK(found, "%s: stack entry not in client list",
				where);
		}
		CHECK(nc == ns, "%s: %d clients but %d in stack", where,
			nc, ns);
		if (m->sel) {
			for (found = 0, c = m->clients; c; c = c->next)
				found |= c == m->sel;
			CHECK(found, "%s: sel not in client list", where);
		}
		CHECK(!m->hidsel || (m->sel && !truehidden(m->sel)),
			"%s: hidsel without a shown selection", where);
		CHECK(m->pertag->curtag <= LENGTH(tags), "%s: curtag", where);
		CHECK(m->pertag->curtag
			? (m->tagset[m->seltags]
			   >> (m->pertag->curtag - 1)) & 1
			: m->tagset[m->seltags] == TAGMASK,
			"%s: curtag %u not in tagset %x", where,
			m->pertag->curtag, m->tagset[m->seltags]);
	}
	if (selmon->sel)
		CHECK(ISVISIBLE(selmon->sel) && !truehidden(selmon->sel),
			"%s: selection is invisible or hidden", where);
	for (i = 0; i < nfw; i++)
		CHECK(wintoclient(fw[i]), "%s: window 0x%lx no longer managed",
			where, fw[i]);
}

static void
fuzz(unsigned int seed, int steps)
{
	Arg a;
	Client *c;
	int step, op, i, x;
	char name[64];
	const char *opname;
	int failbefore = failures;

	srand(seed);
	nfw = 0;
	for (step = 0; step < steps && failures - failbefore < 5; step++) {
		op = rand() % 30;
		a.i = rand() % 2 ? 1 : -1;
		switch (op) {
		case 0: case 1: case 2:
			opname = "map";
			if (nfw < MAXW) {
				snprintf(name, sizeof name, "%.*s",
				    rand() % 60, "window title that is "
				    "fairly long, really quite long indeed");
				fw[nfw++] = mkwin(name);
			}
			break;
		case 3:
			opname = "destroy";
			if (nfw) {
				i = rand() % nfw;
				XDestroyWindow(cdpy, fw[i]);
				fw[i] = fw[--nfw];
			}
			break;
		case 4: opname = "view"; a.ui = 1 << rand() % LENGTH(tags);
			if (!(rand() % 5)) a.ui = rand() % 3 ? 0 : ~0;
			view(&a); break;
		case 5: opname = "toggleview";
			a.ui = 1 << rand() % LENGTH(tags);
			toggleview(&a); break;
		case 6: opname = "tag"; a.ui = 1 << rand() % LENGTH(tags);
			if (!(rand() % 6)) a.ui = ~0;
			tag(&a); break;
		case 7: opname = "toggletag";
			a.ui = 1 << rand() % LENGTH(tags);
			toggletag(&a); break;
		case 8: opname = "focusstackvis"; focusstackvis(&a); break;
		case 9: opname = "focusstackhid"; focusstackhid(&a); break;
		case 10: opname = "hide"; hide(&a); break;
		case 11: opname = "show"; show(&a); break;
		case 12: opname = "showall"; showall(&a); break;
		case 13: opname = "pushring"; pushring(&a); break;
		case 14: opname = "incnmaster"; incnmaster(&a); break;
		case 15: opname = "setlayout";
			a.v = &layouts[rand() % LENGTH(layouts)];
			setlayout(rand() % 4 ? &a : NULL); break;
		case 16: opname = "togglefloating"; togglefloating(&a); break;
		case 17: opname = "zoom"; zoom(&a); break;
		case 18: opname = "shiftview"; shiftview(&a); break;
		case 19: opname = "tagtoright"; tagtoright(&a); break;
		case 20: opname = "tagtoleft"; tagtoleft(&a); break;
		case 21: opname = "clicktitle";
			x = rand() % (selmon->ww + 1);
			clickbar(x, rand() % 3 ? Button1 : Button2); break;
		case 22: opname = "rename";
			if (nfw) {
				snprintf(name, sizeof name, "%.*s",
				    rand() % 60, "renamed renamed renamed "
				    "renamed renamed renamed renamed");
				XStoreName(cdpy, fw[rand() % nfw], name);
			}
			break;
		case 23: opname = "togglebar"; togglebar(&a); break;
		case 24: opname = "setmfact"; a.f = rand() % 2 ? 0.05 : -0.05;
			setmfact(&a); break;
		case 25: opname = "focusmon"; focusmon(&a); break;
		case 26: opname = "tagmon"; tagmon(&a); break;
		case 27: opname = "switchall"; switchall(&a); break;
		case 28: opname = "fullscreen";
			if ((c = selmon->sel))
				setfullscreen(c, !c->isfullscreen);
			break;
		default: opname = "togglewin";
			if (nfw && (c = wintoclient(fw[rand() % nfw]))
			&& c->mon == selmon && ISVISIBLE(c)) {
				a.v = c;
				togglewin(&a);
			}
			break;
		}
		pump();
		invariants(opname, step);
	}
	for (i = 0; i < nfw; i++)
		XDestroyWindow(cdpy, fw[i]);
	nfw = 0;
	pump();
	a.ui = 1;
	for (selmon = mons; selmon; selmon = selmon->next)
		view(&a);
	selmon = mons;
	pump();
}

static void
test_quit_unhides(void)
{
	Window w[5];
	Client *order[5], *c;
	int i;

	viewtag(1);
	for (i = 0; i < 5; i++)
		w[i] = mkwin("q");
	for (i = 0, c = selmon->clients; c; c = c->next)
		if (ISVISIBLE(c))
			order[i++] = c;
	/* hide all but the last client in the list, then focus that one,
	 * so the head of the focus stack is the tail of the client list */
	for (i = 0; i < 4; i++) {
		focus(order[i]);
		hide(NULL);
		pump();
	}
	focus(order[4]);
	pump();
	quit(NULL);
	cleanup();
	XSync(dpy, False);
	for (i = 0; i < 5; i++)
		CHECK(mapstate(w[i]) == IsViewable, "window %d left unmapped "
			"and withdrawn after quit; a restarted dwm "
			"cannot find it", i);
}

int
main(int argc, char *argv[])
{
	int i, nseeds = argc > 1 ? atoi(argv[1]) : 20;

	setvbuf(stdout, NULL, _IONBF, 0);
	/* random bar clicks can hit bindings that spawn programs, such as
	 * a terminal on the status text; make every exec fail so that none
	 * opens on the test display and outlives the harness */
	setenv("PATH", "/nonexistent", 1);
	setlocale(LC_CTYPE, "");
	if (!(dpy = XOpenDisplay(NULL)) || !(cdpy = XOpenDisplay(NULL)))
		die("cannot open display");
	checkotherwm();
	setup();
	scan();
	pump();
	printf("monitors:");
	for (Monitor *m = mons; m; m = m->next)
		printf(" %dx%d+%d", m->mw, m->mh, m->mx);
	printf("\n");

	test_settabwidths();
	test_toggleview_all();
	test_view_prev_consistent();
	test_focusstackhid_nosel();
	test_focusstackhid_focus();
	test_peek_then_close();
	test_attachtop_nmaster0();
	test_title_redraw();
	test_title_click();
	test_pushring();
	test_switchall_swap();
	test_removed_monitor_pertag();
	test_moved_monitor_floats();
	test_efficiency();
	for (i = 0; i < nseeds; i++)
		fuzz(i + 1, 400);
	test_quit_unhides();
	XCloseDisplay(cdpy);
	XCloseDisplay(dpy);
	printf("%d failure(s)\n", failures);
	return failures != 0;
}
