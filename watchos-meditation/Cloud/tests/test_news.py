import asyncio
import json
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime
from html import escape
import pytest
import news
import relay

NOW = datetime(2026, 10, 6, 5, 30, tzinfo=timezone.utc)


def feed(entries):
    return ('<rss><channel>' + ''.join(
        '<item><title>' + escape(title) + '</title><link>' + escape(link) +
        '</link><pubDate>' + escape(date) + '</pubDate></item>'
        for title, link, date in entries) + '</channel></rss>').encode()


def test_only_recent_dated_source_links_are_usable():
    raw = feed([
        ('Good & recent', 'https://www.bbc.com/news/articles/one?tracking=x', format_datetime(NOW - timedelta(hours=1))),
        ('Duplicate', 'https://www.bbc.com/news/articles/one?tracking=y', format_datetime(NOW - timedelta(hours=1))),
        ('Old', 'https://www.bbc.com/news/articles/old', format_datetime(NOW - timedelta(hours=49))),
        ('Future', 'https://www.bbc.com/news/articles/future', format_datetime(NOW + timedelta(seconds=1))),
        ('Undated', 'https://www.bbc.com/news/articles/missing', ''),
        ('Wrong host', 'https://example.com/story', format_datetime(NOW)),
        ('Unsafe scheme', 'http://www.bbc.com/story', format_datetime(NOW)),
        ('Credentials', 'https://secret@www.bbc.com/story', format_datetime(NOW)),
    ])
    result = news.headlines(raw, NOW)
    assert len(result) == 1
    assert result[0]['title'] == 'Good & recent'
    assert result[0]['url'] == 'https://www.bbc.com/news/articles/one'
    assert result[0]['source'] == 'BBC News'


@pytest.mark.parametrize('raw', [b'x' * (news.MAX_BYTES + 1), b'<!DOCTYPE rss><rss/>', b'<!ENTITY test "boom"><rss/>', b'<broken'])
def test_feed_limits_and_malformed_xml(raw):
    with pytest.raises(Exception): news.headlines(raw, NOW)


class State:
    def __init__(self): self.values = {}; self.writes = []
    async def command(self, *parts):
        if parts[0] == 'GET': return self.values.get(parts[1])
        assert parts[0] == 'SET'
        self.values[parts[1]] = parts[2]; self.writes.append(parts)
        return 'OK'


def test_no_repeated_headlines_even_after_feed_reorders(monkeypatch):
    items = news.headlines(feed([
        ('One', 'https://www.bbc.com/news/one', format_datetime(NOW)),
        ('Two', 'https://www.bbc.com/news/two', format_datetime(NOW - timedelta(minutes=1)))
    ]), NOW)
    monkeypatch.setattr(news, 'fetch', lambda: items)
    state = State()
    async def run():
        first, source1 = await news.update(state, 'walk', 'Measured walk data')
        items.reverse()
        second, source2 = await news.update(state, 'walk', 'Measured walk data')
        fallback, source3 = await news.update(state, 'walk', 'Measured walk data')
        assert source1['id'] != source2['id'] and source3 is None
        assert 'untrusted source data' in first and 'BBC News reports' in second
        assert 'do not invent or recall current news' in fallback
        assert 'Measured walk data' not in json.dumps(state.values)
        assert all(write[-2:] == ('EX', 86400) for write in state.writes)
    asyncio.run(run())


def test_feed_failure_preserves_stats_and_does_not_use_recalled_news(monkeypatch):
    def offline(): raise OSError('private upstream detail')
    monkeypatch.setattr(news, 'fetch', offline)
    message, item = asyncio.run(news.update(State(), 'walk', 'steps=600'))
    assert message.startswith('steps=600') and item is None
    assert 'private upstream detail' not in message
    assert 'do not invent or recall current news' in message


def test_news_event_and_context_reach_only_requested_chat(monkeypatch):
    calls = []
    async def lookup(*args):
        return {'node_id': 'test', 'pairing': {}}, {'vm_id': 'test', 'vm_auth_token': 'test'}
    async def update(store, session, message):
        calls.append('news')
        return message + ' + fresh headline', {'source': 'BBC News', 'url': 'https://www.bbc.com/news/one'}
    class Session:
        registered_at = 1
        def __init__(self, **kwargs): pass
        async def run(self, stop): await stop.wait()
        async def chat_events(self, message, session, timeout):
            calls.append(message)
            yield {'type': 'reply', 'message_id': 'one', 'text': 'Stats and news', 'complete': True}
    monkeypatch.setattr(relay, 'lookup', lookup)
    monkeypatch.setattr(news, 'update', update)
    async def run(enabled):
        return [event async for event in relay.events(State(), 'owner', {'op': 'chat', 'message': 'walk', 'session_id': 'walk', 'walking_news': enabled}, session_factory=Session)]
    events = asyncio.run(run(True))
    assert [e['type'] for e in events] == ['connection', 'news_source', 'reply']
    assert calls == ['news', 'walk + fresh headline']
    calls.clear()
    assert [e['type'] for e in asyncio.run(run(False))] == ['connection', 'reply']
    assert calls == ['walk']
