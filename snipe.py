#!/usr/bin/env python3
import argparse
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
from html.parser import HTMLParser

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


class Forms(HTMLParser):
    def __init__(self):
        super().__init__()
        self.forms = []
        self.current = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'form':
            self.current = {'action': a.get('action') or '', 'fields': []}
            self.forms.append(self.current)
        elif self.current is not None and tag == 'input' and a.get('name'):
            kind = (a.get('type') or 'text').lower()
            if kind in ('submit', 'button', 'image', 'file', 'reset'):
                return
            if kind in ('checkbox', 'radio') and 'checked' not in a:
                return
            self.current['fields'].append((a['name'], a.get('value') or ''))

    def handle_endtag(self, tag):
        if tag == 'form':
            self.current = None


def parse_forms(body):
    p = Forms()
    p.feed(body)
    return p.forms


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

    def request(self, path, data=None, headers=None, timeout=20):
        url = urllib.parse.urljoin(BASE, path)
        req = urllib.request.Request(url, data=data, headers={'User-Agent': UA, **(headers or {})})
        try:
            with self.opener.open(req, timeout=timeout) as res:
                return res.geturl(), res.status, res.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            return e.geturl(), e.code, e.read().decode('utf-8', 'replace')

    def time_remaining(self, listing_id):
        _, status, body = self.request(
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
            url, status, body = self.request(path)
            m = re.search(r'<input[^>]*\bid="ListingID"[^>]*\bvalue="(\d+)"', body)
            if status == 200 and m:
                return self.parse_lot(url, body, m.group(1))
        raise SnipeError(f'Couldn’t find a NativeBid lot for {ref!r}')

    def parse_lot(self, url, body, listing_id):
        for k, v in re.findall(r'contextMessageDictionary\["(\w+)"\]\s*=\s*"((?:[^"\\]|\\.)*)"', body):
            self.messages[k] = re.sub(r'\\(.)', r'\1', v)

        def number(pattern):
            m = re.search(pattern + r'[^>]*>\s*\$?\s*<span class="NumberPart">([\d,.]+)</span>', body)
            return money(m.group(1)) if m else None

        title = re.search(r'<h3 class="detail__title">(.*?)<', body, re.S)
        kind = re.search(r'<input[^>]*\bid="ListingType"[^>]*\bvalue="(\w+)"', body)
        status = re.search(r'detail__status-label[^>]*>\s*<span class="label[^"]*">([^<]*)<', body)
        biddermax = re.search(r'class="[^"]*detail__biddermax[^"]*"', body)
        bids = re.search(r'awe-rt-AcceptedListingActionCount"[^>]*>(\d+)<', body)
        my_max = number(r'id="MaxBid"') if biddermax and 'awe-hidden' not in biddermax.group(0) else None
        return {
            'url': url,
            'listing_id': listing_id,
            'type': kind.group(1) if kind else 'Auction',
            'title': strip_tags(title.group(1)) if title else '',
            'price': number(r'class="[^"]*awe-rt-CurrentPrice"'),
            'minimum': number(r'class="[^"]*Bidding_Listing_MinPrice[^"]*"'),
            'my_max': my_max or None,
            'status': strip_tags(status.group(1)) if status else '',
            'bids': int(bids.group(1)) if bids else None,
        }

    def login(self):
        if not self.user or not self.password:
            raise SnipeError('Set NB_USER and NB_PASS (or enter them when prompted).')
        data = urllib.parse.urlencode({
            'username': self.user,
            'password': self.password,
            'rememberMe': 'false',
            'returnUrl': '',
        }).encode()
        url, status, body = self.request('/Account/LogOn', data=data,
                                         headers={'Content-Type': 'application/x-www-form-urlencoded'})
        if '/Account/LogOn' in url or status >= 400:
            err = re.search(r'validation-summary-errors[^>]*>.*?<li[^>]*>(.*?)</li>', body, re.S)
            raise SnipeError('Sign-in failed' + (f': {strip_tags(err.group(1))}' if err else '.'))
        log(f'Signed in as {self.user}')

    def confirm(self, lot, amount):
        query = urllib.parse.urlencode({
            'ListingID': lot['listing_id'],
            'ListingType': lot['type'],
            'BidAmount': f'{amount:.2f}',
        })
        _, status, body = self.request(f'/Listing/ConfirmAction?{query}')
        if status != 200:
            raise SnipeError(f'ConfirmAction returned HTTP {status}')
        forms = parse_forms(body)
        if any('/Account/LogOn' in f['action'] for f in forms):
            raise NotSignedIn()
        form = next((f for f in forms if '/Listing/Action' in f['action']), None)
        if not form:
            err = re.search(r'class="[^"]*(?:validation-summary-errors|alert-danger|alert)[^"]*"[^>]*>(.*?)</div>', body, re.S)
            raise SnipeError('NativeBid didn’t accept that bid' + (f': {strip_tags(err.group(1))}' if err else '.'))
        fields = dict(form['fields'])
        if money(fields.get('BidAmount')) != round(amount, 2) or fields.get('ListingID') != lot['listing_id']:
            raise SnipeError('NativeBid’s confirmation didn’t match this bid, so nothing was placed.')
        total = re.search(r'including[^<]*?up to\s*(\$[\d,]+(?:\.\d+)?)', strip_tags(body), re.I)
        return form['action'], urllib.parse.urlencode(form['fields']), total.group(1) if total else None

    def place(self, action, params):
        _, status, body = self.request(f'{action}?{params}', headers={
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
