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


def describe(lot, left=None):
    log(f'Lot {lot["listing_id"]}: {lot["title"]}')
    log(f'  {lot["status"] or "Status unknown"} · current {fmt(lot["price"])} · next bid {fmt(lot["minimum"])}'
        f' · {lot["bids"] if lot["bids"] is not None else "?"} bids'
        + (f' · your max {fmt(lot["my_max"])}' if lot['my_max'] else '')
        + (f' · ends in {duration(left)}' if left is not None else ''))


def retrying(fn, attempts=5, delay=2):
    for i in range(attempts):
        try:
            return fn()
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            if i == attempts - 1:
                raise
            log(f'Network error ({e}); retrying')
            time.sleep(delay)


def wait_for(nb, lot, lead):
    signed_in_again = False
    while True:
        left = retrying(lambda: nb.time_remaining(lot['listing_id']))
        if left <= 0:
            return False
        until_fire = left - lead
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

    if not args.now and not wait_for(nb, lot, LEAD):
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
