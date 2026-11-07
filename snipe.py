#!/usr/bin/env python3
import argparse
import codecs
import getpass
import html
import http.cookiejar
import json
import os
import re
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = 'https://www.nativebid.com'
UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
SUCCESS = re.compile(r'^(ok|success|actionsuccess)$', re.I)
LEAD = 60


def tls_context():
    ctx = ssl.create_default_context()
    if not ctx.cert_store_stats()['x509_ca'] and os.path.exists('/etc/ssl/cert.pem'):
        ctx.load_verify_locations('/etc/ssl/cert.pem')
    return ctx


TLS = tls_context()


class NotSignedIn(Exception):
    pass


class SnipeError(Exception):
    pass


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


def money(s):
    try:
        return float(str(s).replace(',', '').replace('$', ''))
    except (TypeError, ValueError):
        return None


def strip_tags(s):
    return re.sub(r'\s+', ' ', html.unescape(re.sub(r'<[^>]+>', ' ', s))).strip()


def fmt(n):
    return '—' if n is None else f'${n:,.2f}'


def duration(s):
    s = int(max(0, s))
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    parts = [f'{d}d'] * bool(d) + [f'{h}h'] * bool(h) + [f'{m}m'] * bool(m) + [f'{s}s']
    return ' '.join(parts[:2])


CHUNK = 1024
WINDOW = 2048
SMALL = 8192
NUMBER = r'\s*\$?\s*<span class="NumberPart">([\d,.]+)</span>'


def rules(**patterns):
    return {name: re.compile(p, re.S) for name, p in patterns.items()}


class Scanner:
    rules = {}
    many = set()
    needed = ()

    def __init__(self):
        self.found = {}
        self.buf = ''
        self.base = 0
        self.pos = {}
        self.bytes = 0
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')

    def done(self):
        return all(name in self.found for name in self.needed)

    def read(self, res):
        while not self.done():
            data = res.read(CHUNK)
            if not data:
                break
            self.feed(data)
        return self

    def feed(self, data):
        self.bytes += len(data)
        self.buf += self.decoder.decode(data)
        for name, rx in self.rules.items():
            multi = name in self.many
            if not multi and name in self.found:
                continue
            for m in rx.finditer(self.buf, max(0, self.pos.get(name, 0) - self.base)):
                self.pos[name] = self.base + m.end()
                self.hit(name, m, self.base + m.start())
                if not multi:
                    break
        if len(self.buf) > WINDOW:
            cut = len(self.buf) - WINDOW
            self.buf = self.buf[cut:]
            self.base += cut

    def hit(self, name, m, at):
        self.found[name] = m.group(1) if m.re.groups else m.group(0)


class LotPage(Scanner):
    rules = rules(
        message=r'contextMessageDictionary\["(\w+)"\]\s*=\s*"((?:[^"\\]|\\.)*)"',
        listing_id=r'(?:\bid="ListingID"[^>]*\bvalue|\bdata-listingid)="(\d+)"',
        type=r'\bid="ListingType"[^>]*\bvalue="(\w+)"',
        title=r'<h3 class="detail__title">(.*?)<',
        status=r'detail__status-label[^>]*>\s*<span class="label[^"]*">([^<]*)<',
        price=r'(?:class="[^"]*awe-rt-CurrentPrice"[^>]*>|Winning Bid:)' + NUMBER,
        minimum=r'class="[^"]*Bidding_Listing_MinPrice[^"]*"[^>]*>' + NUMBER,
        biddermax=r'class="([^"]*detail__biddermax[^"]*)"',
        max_bid=r'\bid="MaxBid"[^>]*>' + NUMBER,
        bids=r'>\s*(\d+)\s*(?:</span>)?(?:&nbsp;|\s)*Bid\(s\)',
    )
    many = {'message'}
    needed = ('listing_id', 'type', 'title', 'status', 'price', 'minimum', 'biddermax', 'max_bid', 'bids')

    def __init__(self):
        super().__init__()
        self.messages = {}

    def hit(self, name, m, at):
        if name == 'message':
            self.messages[m.group(1)] = re.sub(r'\\(.)', r'\1', m.group(2))
        else:
            super().hit(name, m, at)

    def lot(self, url):
        f = self.found
        shown = 'biddermax' in f and 'awe-hidden' not in f['biddermax']
        return {
            'url': url,
            'listing_id': f['listing_id'],
            'type': f.get('type', 'Auction'),
            'title': strip_tags(f.get('title', '')),
            'price': money(f.get('price')),
            'minimum': money(f.get('minimum')),
            'my_max': (money(f.get('max_bid')) if shown else None) or None,
            'status': strip_tags(f.get('status', '')),
            'bids': int(f['bids']) if 'bids' in f else None,
        }


class ConfirmPage(Scanner):
    rules = rules(
        signed_in=r'/Account/Logoff',
        logon_form=r'<form[^>]*\baction="/Account/LogOn"',
        form=r'<form[^>]*\baction="([^"]*/Listing/Action[^"]*)"',
        form_end=r'</form>',
        input=r'<input\b[^>]*>',
        total=r'including[^<]*?up to\s*(\$[\d,]+(?:\.\d+)?)(?=[\s<])',
        error=r'class="[^"]*(?:validation-summary-errors|alert-danger|alert)[^"]*"[^>]*>(.*?)</div>',
    )
    many = {'form_end', 'input'}

    def __init__(self):
        super().__init__()
        self.start = None
        self.end = None
        self.fields = []

    def done(self):
        return 'logon_form' in self.found or (
            'signed_in' in self.found and self.end is not None and 'total' in self.found)

    def inside(self, at):
        return self.start is not None and at > self.start and (self.end is None or at < self.end)

    def hit(self, name, m, at):
        if name == 'form_end':
            if self.inside(at) and self.end is None:
                self.end = at
        elif name == 'input':
            if self.inside(at):
                self.add_input(m.group(0))
        else:
            if name == 'form':
                self.start = at
            super().hit(name, m, at)

    def add_input(self, tag):
        a = {k.lower(): html.unescape(v or w)
             for k, v, w in re.findall(r'([\w-]+)(?:\s*=\s*(?:"([^"]*)"|\'([^\']*)\'))?', tag[6:])}
        kind = (a.get('type') or 'text').lower()
        if not a.get('name') or kind in ('submit', 'button', 'image', 'file', 'reset'):
            return
        if kind in ('checkbox', 'radio') and 'checked' not in a:
            return
        self.fields.append((a['name'], a.get('value') or ''))


class LoginPage(Scanner):
    rules = rules(error=r'validation-summary-errors[^>]*>.*?<li[^>]*>(.*?)</li>')
    needed = ('error',)


class NativeBid:
    def __init__(self, user=None, password=None):
        self.user = user
        self.password = password
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar),
            urllib.request.HTTPSHandler(context=TLS),
        )
        self.messages = {}

    def open(self, path, data=None, headers=None, timeout=20):
        url = urllib.parse.urljoin(BASE, path)
        req = urllib.request.Request(url, data=data, headers={'User-Agent': UA, **(headers or {})})
        try:
            return self.opener.open(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            return e

    def scan(self, path, page, **kw):
        with self.open(path, **kw) as res:
            return res.geturl(), res.getcode(), page.read(res)

    def small(self, path, **kw):
        with self.open(path, **kw) as res:
            return res.geturl(), res.getcode(), res.read(SMALL).decode('utf-8', 'replace')

    def time_remaining(self, listing_id):
        _, status, body = self.small(
            '/RealTime/GetTimeRemaining',
            data=json.dumps({'listingId': int(listing_id)}).encode(),
            headers={'Content-Type': 'application/json'},
            timeout=10,
        )
        if status != 200:
            raise SnipeError(f'GetTimeRemaining returned HTTP {status}')
        data = json.loads(body)
        if data.get('error'):
            raise SnipeError(f'GetTimeRemaining error: {data["error"]}')
        return float(data.get('secondsRemaining') or 0)

    def lot(self, ref):
        ref = str(ref).strip()
        candidates = [ref] if ref.startswith('http') or ref.startswith('/') else [
            f'/Listing/Details/{ref}',
            f'/Event/LotDetails/{ref}',
        ]
        for path in candidates:
            url, status, page = self.scan(path, LotPage())
            if status == 200 and 'listing_id' in page.found:
                self.messages.update(page.messages)
                return page.lot(url)
        raise SnipeError(f'Couldn’t find a NativeBid lot for {ref!r}')

    def login(self):
        if not self.user or not self.password:
            raise SnipeError('Set NB_USER and NB_PASS (or enter them when prompted).')
        data = urllib.parse.urlencode({
            'username': self.user,
            'password': self.password,
            'rememberMe': 'false',
            'returnUrl': '',
        }).encode()
        with self.open('/Account/LogOn', data=data,
                       headers={'Content-Type': 'application/x-www-form-urlencoded'}) as res:
            if '/Account/LogOn' in res.geturl() or res.getcode() >= 400:
                err = LoginPage().read(res).found.get('error')
                raise SnipeError('Sign-in failed' + (f': {strip_tags(err)}' if err else '.'))
        log(f'Signed in as {self.user}')

    def confirm(self, lot, amount):
        query = urllib.parse.urlencode({
            'ListingID': lot['listing_id'],
            'ListingType': lot['type'],
            'BidAmount': f'{amount:.2f}',
        })
        _, status, page = self.scan(f'/Listing/ConfirmAction?{query}', ConfirmPage())
        if status != 200:
            raise SnipeError(f'ConfirmAction returned HTTP {status}')
        if 'logon_form' in page.found:
            raise NotSignedIn()
        if 'form' not in page.found:
            err = page.found.get('error')
            raise SnipeError('NativeBid didn’t accept that bid' + (f': {strip_tags(err)}' if err else '.'))
        fields = dict(page.fields)
        if money(fields.get('BidAmount')) != round(amount, 2) or fields.get('ListingID') != lot['listing_id']:
            raise SnipeError('NativeBid’s confirmation didn’t match this bid, so nothing was placed.')
        return page.found['form'], urllib.parse.urlencode(page.fields), page.found.get('total')

    def place(self, action, params):
        _, status, body = self.small(f'{action}?{params}', headers={
            'X-Requested-With': 'XMLHttpRequest',
            'Accept': 'application/json',
        })
        body = body.strip()
        try:
            result = json.loads(body)
        except ValueError:
            result = body
        if isinstance(result, dict):
            result = result.get('ReasonCode') or result.get('Status') or result.get('Message') or json.dumps(result)
        result = str(result or '')
        if status == 200 and SUCCESS.match(result):
            return True, result
        readable = self.messages.get(result) or (result if result and not re.search(r'[<{]', result) else '')
        return False, readable or f'NativeBid didn’t accept that bid (HTTP {status}).'


def notify(topic, title, message):
    if not topic:
        return
    try:
        req = urllib.request.Request(f'https://ntfy.sh/{topic}', data=message.encode(), headers={'Title': title})
        urllib.request.urlopen(req, timeout=10, context=TLS).close()
    except Exception as e:
        log(f'Notification failed: {e}')


def summary(lot, left=None):
    return (f'{lot["status"] or "Status unknown"} · current {fmt(lot["price"])} · next bid {fmt(lot["minimum"])}'
            f' · {lot["bids"] if lot["bids"] is not None else "?"} bids'
            + (f' · your max {fmt(lot["my_max"])}' if lot['my_max'] else '')
            + (f' · ends in {duration(left)}' if left is not None else ''))


def describe(lot, left=None):
    log(f'Lot {lot["listing_id"]}: {lot["title"]}')
    log(f'  {summary(lot, left)}')


def retrying(fn, attempts=5, delay=2):
    for i in range(attempts):
        try:
            return fn()
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            if i == attempts - 1:
                raise
            log(f'Network error ({e}); retrying')
            time.sleep(delay)


def wait_for(nb, listing_id, lead, give_up=None):
    signed_in_again = False
    while True:
        left = retrying(lambda: nb.time_remaining(listing_id))
        if left <= 0:
            return False
        until_fire = left - lead
        if give_up is not None and until_fire > give_up:
            return None
        if until_fire <= 1.5:
            time.sleep(max(0, until_fire))
            return True
        if not signed_in_again and until_fire <= 360:
            retrying(nb.login)
            signed_in_again = True
        pause = min(300, max(0.5, until_fire / 2))
        if pause >= 5:
            log(f'Ends in {duration(left)}; bidding in {duration(until_fire)}')
        time.sleep(pause)


def fire(nb, lot, max_bid):
    for attempt in range(3):
        try:
            lot = retrying(lambda: nb.lot(lot['url']))
            if lot['my_max'] and lot['my_max'] >= max_bid:
                return True, f'Your max is already {fmt(lot["my_max"])}'
            if lot['minimum'] is not None and lot['minimum'] > max_bid:
                return False, f'Next bid is {fmt(lot["minimum"])}, above your max of {fmt(max_bid)}'
            try:
                action, params, total = nb.confirm(lot, max_bid)
            except NotSignedIn:
                nb.login()
                action, params, total = nb.confirm(lot, max_bid)
            log(f'Placing max bid {fmt(max_bid)} (up to {total or "?"} with fees)')
            return nb.place(action, params)
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            log(f'Network error while bidding ({e}); checking and retrying')
            time.sleep(1)
    return False, 'Couldn’t reach NativeBid; the bid may not have gone through.'


def follow_up(nb, lot, topic):
    while True:
        try:
            left = nb.time_remaining(lot['listing_id'])
        except Exception as e:
            log(f'Couldn’t check time ({e})')
            left = 5
        if left <= 0:
            break
        time.sleep(min(30, max(1, left / 2)))
    time.sleep(3)
    final = retrying(lambda: nb.lot(lot['url']))
    describe(final)
    summary = f'{final["status"] or "Ended"} at {fmt(final["price"])}'
    log(summary)
    notify(topic, f'Lot ended: {final["title"][:60]}', summary)


def cmd_info(args):
    nb = NativeBid()
    lot = nb.lot(args.lot)
    describe(lot, nb.time_remaining(lot['listing_id']))
    log(f'  {lot["url"]}')


def cmd_snipe(args):
    user = os.environ.get('NB_USER') or input('NativeBid username: ')
    password = os.environ.get('NB_PASS') or getpass.getpass('NativeBid password: ')
    topic = os.environ.get('NB_NTFY_TOPIC')
    nb = NativeBid(user, password)

    lot = nb.lot(args.lot)
    left = nb.time_remaining(lot['listing_id'])
    describe(lot, left)
    if left <= 0:
        raise SnipeError('This lot has already ended.')
    if lot['minimum'] is not None and args.max < lot['minimum']:
        raise SnipeError(f'Your max {fmt(args.max)} is below the next bid {fmt(lot["minimum"])}.')

    nb.login()
    _, _, total = nb.confirm(lot, args.max)
    log(f'Ready: max bid {fmt(args.max)} (up to {total or "?"} with fees), '
        + ('placing now' if args.now else f'{LEAD}s before the end'))
    notify(topic, 'Snipe scheduled', f'{lot["title"][:80]}\nMax {fmt(args.max)}, ends in {duration(left)}')

    if not args.now and not wait_for(nb, lot['listing_id'], LEAD):
        log('Lot ended before the bid time')
        notify(topic, 'Snipe missed', f'{lot["title"][:80]} ended before the bid was placed')
        return 1

    ok, message = fire(nb, lot, args.max)
    log(('✓ ' if ok else '✗ ') + message)
    notify(topic, 'Bid placed' if ok else 'Bid failed', f'{lot["title"][:80]}\n{message}')
    if not ok:
        return 1
    if not args.now:
        follow_up(nb, lot, topic)
    return 0


BID_WINDOW = 15
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'snipes.json')
HELP = ('<b>Commands</b>\n'
        '/snipe or /s <i>lot max</i> – set or change a snipe\n'
        '/list or /l – your snipes\n'
        '/cancel or /c <i># or lot</i> – cancel a snipe\n'
        '/info or /i <i>lot</i> – price and time left\n\n'
        '<i>lot</i> is a lot link or number, e.g.\n'
        '<code>/snipe 266333831 10</code>')
USAGE = {
    '/snipe': (2, '/snipe <lot> <max>, e.g. /snipe 266333831 10'),
    '/list': (0, '/list'),
    '/cancel': (1, '/cancel <# or lot>, e.g. /cancel 2 or /cancel 266333831'),
    '/info': (1, '/info <lot>'),
}
ALIASES = {'/s': '/snipe', '/l': '/list', '/c': '/cancel', '/i': '/info'}
NETWORK = (urllib.error.URLError, TimeoutError, ConnectionError, OSError)


def esc(s):
    return html.escape(str(s), quote=False)


def link(url, title):
    return f'<a href="{html.escape(url)}">{esc(title[:100] or "Lot")}</a>'


def plain(text):
    return html.unescape(re.sub(r'<[^>]+>', '', text))


def button(label, data):
    return {'text': label, 'callback_data': data}


def when(t):
    return time.strftime('%a %b %-d, %-I:%M:%S %p', time.localtime(t))


def prices(lot):
    bids = lot['bids'] if lot['bids'] is not None else '?'
    return f'Current <b>{fmt(lot["price"])}</b> · Next bid <b>{fmt(lot["minimum"])}</b> · {bids} bids'


class Telegram:
    def __init__(self, token, chat_id):
        self.url = f'https://api.telegram.org/bot{token}/'
        self.chat_id = int(chat_id)

    def call(self, method, params, wait=15):
        req = urllib.request.Request(self.url + method, data=json.dumps(params).encode(),
                                     headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=wait, context=TLS) as res:
                return json.loads(res.read().decode('utf-8', 'replace'))['result']
        except urllib.error.HTTPError as e:
            try:
                reason = json.loads(e.read().decode('utf-8', 'replace')).get('description')
            except ValueError:
                reason = None
            if e.code == 409:
                log('Telegram conflict: something else is reading this bot’s messages')
            raise SnipeError(f'Telegram {method} failed (HTTP {e.code}): {reason or "no details"}')

    def send(self, text, buttons=None, edit=None):
        params = {'chat_id': self.chat_id, 'text': text[:4000], 'parse_mode': 'HTML',
                  'link_preview_options': {'is_disabled': True}}
        if buttons is not None:
            params['reply_markup'] = {'inline_keyboard': buttons}
        if not edit:
            self.call('sendMessage', params)
            return
        try:
            self.call('editMessageText', {**params, 'message_id': edit})
        except SnipeError as e:
            if 'not modified' not in str(e):
                raise

    def answer(self, query_id, text=None, alert=False):
        params = {'callback_query_id': query_id}
        if text:
            params.update(text=text[:200], show_alert=alert)
        self.call('answerCallbackQuery', params)

    def updates(self, offset, wait):
        params = {'timeout': wait, 'limit': 5, 'allowed_updates': ['message', 'callback_query']}
        if offset is not None:
            params['offset'] = offset
        return self.call('getUpdates', params, wait=wait + 15)


class Bot:
    def __init__(self, nb, tg):
        self.nb = nb
        self.tg = tg
        self.outbox = []
        try:
            with open(STATE) as f:
                self.state = json.load(f)
        except FileNotFoundError:
            self.state = {'offset': None, 'next_id': 1, 'snipes': []}

    @property
    def snipes(self):
        return self.state['snipes']

    def pending(self):
        return [s for s in self.snipes if s['state'] == 'pending']

    def save(self):
        tmp = STATE + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, STATE)

    def say(self, text, buttons=None, edit=None):
        log('→ ' + plain(text).replace('\n', ' | '))
        self.outbox.append((text, buttons, edit))

    def flush(self):
        while self.outbox:
            text, buttons, edit = self.outbox[0]
            try:
                self.tg.send(text, buttons, edit)
            except SnipeError as e:
                if 'HTTP 400' not in str(e):
                    raise
                log(f'{e}; sending as plain text')
                self.tg.call('sendMessage', {'chat_id': self.tg.chat_id, 'text': plain(text)[:4000]})
            self.outbox.pop(0)

    def drop(self, snipe):
        self.snipes.remove(snipe)
        self.save()

    def find(self, ref):
        ref = str(ref).strip()
        sid = ref[1:] if ref.startswith('#') else ref
        s = next((s for s in self.snipes if str(s['id']) == sid), None)
        if not s and not ref.startswith('#'):
            s = self.by_lot(ref)
        if not s:
            raise SnipeError(f'There’s no snipe matching {ref}. Send /list to see your snipes.')
        if s['state'] != 'pending':
            raise SnipeError(f'Snipe #{s["id"]}’s bid has already been placed.')
        return s

    def by_lot(self, ref):
        numbers = set(re.findall(r'\d{5,}', ref))
        for s in self.snipes:
            if numbers & ({s['listing_id']} | set(re.findall(r'\d{5,}', s['url']))):
                return s
        if not numbers and not ref.startswith(('http', '/')):
            return None
        try:
            listing_id = self.nb.lot(ref)['listing_id']
        except SnipeError:
            return None
        return next((s for s in self.snipes if s['listing_id'] == listing_id), None)

    def run(self):
        self.nb.login()
        count = len(self.pending())
        self.say(f'🤖 <b>Bot started</b> · {count} snipe{"" if count == 1 else "s"} pending\n\n{HELP}')
        while True:
            try:
                self.tick()
            except KeyboardInterrupt:
                raise
            except Exception as e:
                log(f'Error: {e}')
                time.sleep(5)

    def tick(self):
        self.refresh()
        now = time.time()
        due = [s for s in self.pending() if now >= s['ends'] - LEAD - BID_WINDOW]
        if due:
            self.bid_mode(min(due, key=lambda s: s['ends']))
            return
        self.flush()
        wait = self.next_event() - time.time()
        if wait < 1:
            time.sleep(max(0, wait))
            return
        self.poll(int(min(25, wait)))

    def next_event(self):
        times = [time.time() + 25]
        for s in self.snipes:
            if s['state'] == 'pending':
                times += [s['ends'] - LEAD - BID_WINDOW, s['check_at']]
            elif s['state'] == 'placed':
                times.append(s['check_at'])
        return min(times)

    def missed(self, s):
        self.say(f'⚠️ <b>Snipe missed</b> · #{s["id"]}\n{link(s["url"], s["title"])}\n\n'
                 'The lot ended before the bid went out.')
        self.drop(s)

    def refresh(self):
        now = time.time()
        for s in list(self.snipes):
            if now < s['check_at']:
                continue
            left = self.nb.time_remaining(s['listing_id'])
            now = time.time()
            if s['state'] == 'pending':
                if left <= 0:
                    self.missed(s)
                    continue
                s['ends'] = now + left
                s['check_at'] = now + min(300, max(5, (left - LEAD - BID_WINDOW) / 2))
            elif left > 0:
                s['ends'] = now + left
                s['check_at'] = now + min(30, max(1, left / 2))
            else:
                time.sleep(3)
                final = self.nb.lot(s['url'])
                self.say(f'🏁 <b>Lot ended</b> · #{s["id"]}\n{link(s["url"], s["title"])}\n\n'
                         f'<b>{esc(final["status"] or "Ended")}</b> at <b>{fmt(final["price"])}</b>\n'
                         f'Your max: {fmt(s["max"])}')
                self.drop(s)
                continue
            self.save()

    def bid_mode(self, s):
        log(f'Bid mode for #{s["id"]}; Telegram paused')
        ready = wait_for(self.nb, s['listing_id'], LEAD, give_up=BID_WINDOW * 2)
        if ready is None:
            log(f'#{s["id"]} was extended; leaving bid mode')
            s['check_at'] = 0
            self.save()
            return
        if not ready:
            self.missed(s)
            return
        ok, message = fire(self.nb, {'url': s['url']}, s['max'])
        log(('✓ ' if ok else '✗ ') + message)
        title = link(s['url'], s['title'])
        if ok:
            self.say(f'✅ <b>Bid placed</b> · #{s["id"]}\n{title}\n\n'
                     f'Your max: <b>{fmt(s["max"])}</b>\n<i>NativeBid: {esc(message)}</i>')
            s['state'] = 'placed'
            s['check_at'] = time.time() + LEAD
            self.save()
        else:
            self.say(f'❌ <b>Bid failed</b> · #{s["id"]}\n{title}\n\n<b>{esc(message)}</b>')
            self.drop(s)

    def poll(self, wait):
        for u in self.tg.updates(self.state['offset'], wait):
            self.state['offset'] = u['update_id'] + 1
            self.save()
            if 'callback_query' in u:
                q = u['callback_query']
                if (q.get('from') or {}).get('id') != self.tg.chat_id:
                    log(f'Ignored a button press from {(q.get("from") or {}).get("id")}')
                    continue
                log(f'← [button] {q.get("data")}')
                self.press(q)
            else:
                m = u.get('message') or {}
                chat = (m.get('chat') or {}).get('id')
                if chat != self.tg.chat_id:
                    log(f'Ignored a message from chat {chat}')
                    continue
                log(f'← {m.get("text") or ""}')
                self.handle(m.get('text') or '')
            self.flush()

    def handle(self, text):
        parts = text.split()
        cmd = parts[0].split('@')[0].lower() if parts else ''
        cmd = ALIASES.get(cmd, cmd)
        args = parts[1:]
        try:
            if cmd in ('/start', '/help') or not cmd.startswith('/'):
                self.say(HELP)
                return
            if cmd not in USAGE:
                self.say(f'❌ I don’t know <b>{esc(parts[0])}</b>.\n\n{HELP}')
                return
            count, form = USAGE[cmd]
            if len(args) != count:
                extra = f' I didn’t understand "{" ".join(args[count:])}".' if len(args) > count else ''
                raise SnipeError(f'Usage: {form}.{extra}')
            if cmd == '/snipe':
                max_bid = money(args[1])
                if not max_bid or max_bid <= 0:
                    raise SnipeError(f'"{args[1]}" isn’t a dollar amount. Usage: {form}')
                self.add(args[0], max_bid)
            elif cmd == '/list':
                self.show()
            elif cmd == '/cancel':
                self.cancel(args[0])
            else:
                self.info(args[0])
        except SnipeError as e:
            self.say(f'❌ {esc(e)}')
        except NETWORK as e:
            self.say(f'❌ Couldn’t reach NativeBid ({esc(e)}). Try again.')

    def press(self, q):
        msg = (q.get('message') or {}).get('message_id')
        kind, _, rest = (q.get('data') or '').partition(':')
        note = None
        try:
            if kind == 'bump':
                sid, delta = rest.split(':')
                s = self.find(sid)
                self.add(s['url'], round(s['max'] + float(delta), 2), edit=msg)
                note = f'Max is now {fmt(s["max"])}'
            elif kind == 'card':
                self.card(self.find(rest), edit=msg)
            elif kind == 'open':
                self.card(self.find(rest))
            elif kind == 'cancel':
                self.cancel(rest, edit=msg)
                note = f'Snipe #{rest} cancelled'
            elif kind == 'listcancel':
                self.cancel(rest, quiet=True)
                self.show(edit=msg)
                note = f'Snipe #{rest} cancelled'
            elif kind == 'list':
                self.show(edit=msg)
            elif kind == 'info':
                self.info(rest, edit=msg)
            self.tg.answer(q['id'], note)
        except SnipeError as e:
            self.tg.answer(q['id'], str(e), alert=True)
        except NETWORK as e:
            self.tg.answer(q['id'], f'Couldn’t reach NativeBid ({e}). Try again.', alert=True)

    def confirm(self, lot, amount):
        try:
            return self.nb.confirm(lot, amount)[2]
        except NotSignedIn:
            self.nb.login()
            return self.nb.confirm(lot, amount)[2]

    def add(self, ref, max_bid, edit=None):
        if not max_bid or max_bid <= 0:
            raise SnipeError(f'Usage: {USAGE["/snipe"][1]}')
        lot = self.nb.lot(ref)
        left = self.nb.time_remaining(lot['listing_id'])
        if left <= 0:
            raise SnipeError('That lot has already ended.')
        if lot['minimum'] is not None and max_bid < lot['minimum']:
            raise SnipeError(f'Your max {fmt(max_bid)} is below the next bid {fmt(lot["minimum"])}.')
        same = next((s for s in self.snipes if s['listing_id'] == lot['listing_id']), None)
        if same and same['state'] != 'pending':
            raise SnipeError(f'Snipe #{same["id"]}’s bid of {fmt(same["max"])} has already been placed, '
                             'so it can’t be changed here.')
        if same and same['max'] == max_bid:
            raise SnipeError(f'Snipe #{same["id"]} is already set to {fmt(max_bid)}.')
        total = self.confirm(lot, max_bid)
        now = time.time()
        if same:
            old = same['max']
            same.update(max=max_bid, total=total, ends=now + left)
            self.save()
            self.card(same, lot, left, f'✏️ <b>Snipe #{same["id"]} updated</b>', old, edit)
            return
        s = {
            'id': self.state['next_id'],
            'url': lot['url'],
            'listing_id': lot['listing_id'],
            'title': lot['title'],
            'max': max_bid,
            'total': total,
            'state': 'pending',
            'ends': now + left,
            'check_at': now + min(300, max(5, (left - LEAD - BID_WINDOW) / 2)),
        }
        self.state['next_id'] += 1
        self.snipes.append(s)
        self.save()
        self.card(s, lot, left, f'🎯 <b>Snipe #{s["id"]} scheduled</b>', edit=edit)

    def card(self, s, lot=None, left=None, heading=None, old=None, edit=None):
        if lot is None:
            lot = self.nb.lot(s['url'])
            left = self.nb.time_remaining(s['listing_id'])
            s['ends'] = time.time() + left
            self.save()
        mx = f'<b>{fmt(s["max"])}</b>'
        if old is not None:
            mx = f'<s>{fmt(old)}</s> → {mx}'
        sid = s['id']
        text = (f'{heading or f"🎯 <b>Snipe #{sid}</b>"}\n{link(s["url"], s["title"])}\n\n'
                f'Your max: {mx}\n'
                f'<i>Up to {esc(s.get("total") or "?")} with fees</i>\n\n'
                f'{prices(lot)}\n'
                f'Ends <b>{when(s["ends"])}</b>\n'
                f'In {duration(left)} · bidding {LEAD}s before the end')
        buttons = [
            [button(f'{"−" if d < 0 else "+"}${abs(d)}', f'bump:{sid}:{d}') for d in (-5, -1, 1, 5)],
            [button('🔄 Refresh', f'card:{sid}'), button('✕ Cancel', f'cancel:{sid}')],
        ]
        self.say(text, buttons, edit)

    def show(self, edit=None):
        refresh = [button('🔄 Refresh', 'list')]
        if not self.snipes:
            self.say('📋 <b>No snipes</b>\n\nSet one with <code>/snipe lot max</code>.', [refresh], edit)
            return
        now = time.time()
        lines = ['📋 <b>Your snipes</b>']
        buttons = []
        for s in sorted(self.snipes, key=lambda s: s['ends']):
            lines.append(f'\n<b>#{s["id"]}</b> · {link(s["url"], s["title"][:60])}')
            if s['state'] == 'pending':
                lines.append(f'Max <b>{fmt(s["max"])}</b> · ends in <b>{duration(s["ends"] - now)}</b>')
                buttons.append([button(f'⚙️ Edit #{s["id"]}', f'open:{s["id"]}'),
                                button(f'✕ Cancel #{s["id"]}', f'listcancel:{s["id"]}')])
            else:
                lines.append(f'Max <b>{fmt(s["max"])}</b> · ✅ bid placed, waiting for the end')
        self.say('\n'.join(lines), buttons + [refresh], edit)

    def cancel(self, ref, edit=None, quiet=False):
        s = self.find(ref)
        self.drop(s)
        if not quiet:
            self.say(f'🗑 <b>Snipe #{s["id"]} cancelled</b>\n{link(s["url"], s["title"])}\n\n'
                     f'Max was {fmt(s["max"])}.', [], edit)

    def info(self, ref, edit=None):
        lot = self.nb.lot(ref)
        left = self.nb.time_remaining(lot['listing_id'])
        ends = f'Ends <b>{when(time.time() + left)}</b>\nIn {duration(left)}' if left > 0 else '<b>Ended</b>'
        mine = next((s for s in self.snipes if s['listing_id'] == lot['listing_id']), None)
        text = (f'ℹ️ {link(lot["url"], lot["title"])}\n\n'
                f'Status: <b>{esc(lot["status"] or "unknown")}</b>\n'
                f'{prices(lot)}\n'
                f'{ends}')
        if lot['my_max']:
            text += f'\n\nYour max on the site: <b>{fmt(lot["my_max"])}</b>'
        if mine:
            text += f'\n\n🎯 Snipe #{mine["id"]} is set at <b>{fmt(mine["max"])}</b>'
        self.say(text, [[button('🔄 Refresh', f'info:{lot["listing_id"]}')]], edit)


def cmd_bot(args):
    token = os.environ.get('TG_TOKEN')
    chat = os.environ.get('TG_CHAT_ID')
    user = os.environ.get('NB_USER')
    password = os.environ.get('NB_PASS')
    if not (token and chat and user and password):
        raise SnipeError('Set TG_TOKEN, TG_CHAT_ID, NB_USER and NB_PASS in .env first.')
    Bot(NativeBid(user, password), Telegram(token, chat)).run()


def load_env(path=os.path.join(os.path.dirname(os.path.abspath(__file__)), '.env')):
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        return
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
            value = value[1:-1]
        if value and key not in os.environ:
            os.environ[key] = value


def main():
    load_env()
    p = argparse.ArgumentParser(description='Place a NativeBid max bid shortly before a lot ends.')
    sub = p.add_subparsers(dest='cmd', required=True)

    info = sub.add_parser('info', help='Show a lot’s price, next bid and time left (no sign-in needed)')
    info.add_argument('lot', help='Lot URL, lot number from the URL, or listing ID')
    info.set_defaults(fn=cmd_info)

    snipe = sub.add_parser('snipe', help='Wait, then place your max bid near the end')
    snipe.add_argument('lot', help='Lot URL, lot number from the URL, or listing ID')
    snipe.add_argument('max', type=float, help='Your maximum bid in dollars')
    snipe.add_argument('--now', action='store_true', help='Bid immediately instead of waiting (for testing)')
    snipe.set_defaults(fn=cmd_snipe)

    bot = sub.add_parser('bot', help='Run the Telegram bot that takes /snipe commands from your phone')
    bot.set_defaults(fn=cmd_bot)

    args = p.parse_args()
    try:
        sys.exit(args.fn(args) or 0)
    except SnipeError as e:
        log(f'✗ {e}')
        sys.exit(1)
    except KeyboardInterrupt:
        log('Cancelled')
        sys.exit(130)


if __name__ == '__main__':
    main()
