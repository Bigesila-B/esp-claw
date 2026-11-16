import { createSignal, onCleanup } from 'solid-js';
import { createStore } from 'solid-js/store';
import { createWebimSession, deleteWebimSession, fetchWebimHistory, fetchWebimSession, fetchWebimSessions, renameWebimSession, sendWebimMessage, webimWebSocketUrl, type WebImMessage, type WebImSession } from '../api/client';
import { t } from '../i18n';

type Thread = { messages: WebImMessage[]; live: WebImMessage[]; draft: string; files: string[]; before: number; more: boolean; loading: boolean; scroll: number; loaded: boolean; error: string; pending?: { id: string; text: string; files: string[]; state: 'sending' | 'accepted' | 'failed' }; run: WebImSession['run_state']; delivery: string };
const emptyThread = (): Thread => ({ messages: [], live: [], draft: '', files: [], before: 0, more: false, loading: false, scroll: -1, loaded: false, error: '', run: 'idle', delivery: '' });
const STORAGE_KEY = 'esp-claw-active-session';
const LEGACY_CHAT_KEY = 'esp-claw-webim-chat-id';

export function createWebChat() {
  const [sessions, setSessions] = createSignal<WebImSession[]>([]);
  const [active, setActive] = createSignal('');
  const [source, setSource] = createSignal('all');
  const [more, setMore] = createSignal(false);
  const [online, setOnline] = createSignal(true);
  const [error, setError] = createSignal('');
  const [threads, setThreads] = createStore<Record<string, Thread>>({ '': emptyThread() });
  let cursor = 0, boot = '', disposed = false, refreshing = false, creating = false;
  let listGeneration = 0;
  let socket: WebSocket | undefined;
  const generations = new Map<string, number>();
  const thread = (): Thread => threads[active()] ?? threads['']!;
  const session = () => sessions().find(s => s.session === active());
  const ensure = (id: string) => { if (!threads[id]) setThreads(id, emptyThread()); };
  const remember = (id: string) => { try { localStorage.setItem(STORAGE_KEY, id); } catch { /* Storage can be disabled. */ } };
  const checkBoot = (next: string) => {
    if (boot && boot !== next) {
      for (const id of Object.keys(threads)) {
        const pending = threads[id]!.pending;
        setThreads(id, { messages: [], live: [], loaded: false, loading: false, run: 'idle', more: false, pending: pending ? { ...pending, state: 'failed' } : undefined });
        generations.set(id, (generations.get(id) ?? 0) + 1);
      }
      setError(t('chatRestarted') as string);
    }
    boot = next;
  };
  const list = async (append = false) => {
    const generation = ++listGeneration;
    const page = await fetchWebimSessions(source(), append ? cursor : 0);
    if (disposed || generation !== listGeneration) return;
    checkBoot(page.boot_id);
    cursor = page.next_cursor; setMore(page.has_more);
    const old = sessions();
    const retained = !append && !page.has_more ? old.filter(s => source() !== 'all' && s.source !== source() || s.session === active() && !threads[s.session]?.loaded) : old;
    const merged = new Map(retained.map(s => [s.session, s]));
    page.items.forEach(s => merged.set(s.session, s));
    setSessions([...merged.values()].sort((a, b) => b.activity_order - a.activity_order || a.session.localeCompare(b.session)));
    setOnline(true); setError('');
  };
  const load = async (id: string, older = false) => {
    if (!id || disposed) return;
    ensure(id);
    if (threads[id]!.loading) return;
    const generation = (generations.get(id) ?? 0) + 1;
    generations.set(id, generation);
    setThreads(id, { loading: true });
    try {
      const page = await fetchWebimHistory(id, older ? threads[id]!.before : 0);
      if (disposed || generations.get(id) !== generation) return;
      checkBoot(page.boot_id);
      const current = threads[id]!;
      const previous = new Map(current.messages.map(message => [message.seq, message]));
      const rebuilt = (current.messages.at(-1)?.seq ?? 0) > page.total ||
        page.messages.some(message => {
          const old = previous.get(message.seq);
          return old && (old.role !== message.role || old.text !== message.text);
        });
      if (rebuilt) {
        previous.clear();
        if (older) {
          setThreads(id, { messages: [], live: [], loading: false, loaded: false });
          await load(id);
          return;
        }
      }
      const messages = previous;
      page.messages.forEach(m => messages.set(m.seq, m));
      const pending = threads[id]!.pending;
      setThreads(id, {
        messages: [...messages.values()].filter(m => m.seq <= page.total).sort((a, b) => a.seq - b.seq),
        loaded: true, loading: false, error: '', run: page.run_state, delivery: page.delivery_state ?? '',
        ...(rebuilt ? { live: [] } : {}),
        ...(older || !current.loaded || rebuilt ? { before: page.next_before, more: page.has_more } : {}),
        pending: pending?.state === 'accepted' && page.run_state === 'idle' ? undefined : pending,
      });
      setOnline(true);
    } catch (e) {
      if (!disposed && generations.get(id) === generation) {
        const message = (e as Error).message;
        setThreads(id, { loading: false, error: message });
        if ((e as Error & { code?: string }).code === 'session_not_found') {
          setSessions(prev => prev.filter(s => s.session !== id));
          if (active() === id) {
            const next = sessions()[0]?.session ?? '';
            ensure(next); setActive(next); remember(next); void load(next);
          }
        }
      }
    }
  };
  const select = async (id: string) => {
    ensure(id); setActive(id); remember(id);
    await load(id);
  };
  const fresh = () => { setActive(''); remember(''); };
  const refresh = async () => {
    if (refreshing || disposed) return;
    refreshing = true;
    try { await list(); await load(active()); }
    catch (e) { if (!disposed) { setOnline(false); setError((e as Error).message); } }
    finally { refreshing = false; }
  };
  const send = async (retry = false, command?: string) => {
    let id = active();
    if (retry && !thread().pending) return;
    if (creating || thread().pending?.state === 'sending') return;
    const draft = thread();
    const text = command ?? (retry ? draft.pending?.text ?? '' : draft.draft.trim());
    const files = command ? [] : retry ? draft.pending?.files ?? [] : [...draft.files];
    if (!text && !files.length) return;
    const messageId = retry ? draft.pending!.id : globalThis.crypto?.randomUUID?.() ?? `w-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    try {
      if (!id) {
        creating = true;
        const created = await createWebimSession();
        if (disposed) return;
        id = created.session;
        setThreads(id, { ...emptyThread(), draft: text, files });
        setThreads('', emptyThread());
        setSessions(prev => [created, ...prev]);
        if (active() === '') { setActive(id); remember(id); }
      }
      generations.set(id, (generations.get(id) ?? 0) + 1);
      setThreads(id, { loading: false, ...(command ? {} : { draft: '', files: [] }), error: '',
        pending: { id: messageId, text, files, state: 'sending' } });
      const receipt = await sendWebimMessage(id, messageId, text, files);
      if (disposed) return;
      checkBoot(receipt.boot_id);
      setThreads(id, { run: 'queued', pending: { id: messageId, text, files, state: 'accepted' } });
      await load(id);
    } catch (e) {
      if (!disposed) { ensure(id); setThreads(id, { ...(!threads[id]!.draft ? { draft: text, files } : {}), error: (e as Error).message, pending: { id: messageId, text, files, state: 'failed' } }); }
    } finally { creating = false; }
  };
  const rename = async (id: string, title: string) => {
    const updated = await renameWebimSession(id, title);
    if (!disposed) setSessions(prev => prev.map(s => s.session === id ? updated : s));
  };
  const remove = async (id: string) => {
    await deleteWebimSession(id);
    if (disposed) return;
    generations.set(id, (generations.get(id) ?? 0) + 1);
    setSessions(prev => prev.filter(s => s.session !== id));
    if (active() === id) await select(sessions()[0]?.session ?? '');
    await list();
  };
  const connect = () => {
    if (disposed || (socket && socket.readyState < WebSocket.CLOSING)) return;
    socket = new WebSocket(webimWebSocketUrl());
    socket.onopen = () => { socket?.send(JSON.stringify({ type: 'subscribe', scope: 'webim' })); void refresh(); };
    socket.onmessage = ({ data }) => {
      try {
        const notice = JSON.parse(data);
        if (notice.type === 'chat.message' && typeof notice.session === 'string' &&
            typeof notice.text === 'string' && sessions().some(session => session.session === notice.session)) {
          const id: string = notice.session;
          ensure(id);
          const current = threads[id]!;
          const message: WebImMessage = {
            role: 'assistant',
            text: notice.text,
            seq: current.messages.at(-1)?.seq ?? 0,
          };
          if (typeof notice.link_url === 'string') {
            message.links = [{ url: notice.link_url, label: '' }];
          }
          setThreads(id, 'live', [...current.live.slice(-15), message]);
        }
      } catch {
        // A lost notification can be recovered by pulling persisted history.
      }
      void refresh();
    };
    socket.onerror = () => socket?.close();
  };
  const start = async () => {
    try {
      await list();
      let saved = '', legacy = '';
      try {
        saved = localStorage.getItem(STORAGE_KEY) ?? '';
        legacy = localStorage.getItem(LEGACY_CHAT_KEY) ?? '';
      } catch { /* Optional preference. */ }
      if (saved && !sessions().some(s => s.session === saved)) {
        try { const found = await fetchWebimSession(saved); setSessions(prev => [...prev, found]); }
        catch { saved = ''; }
      }
      const prior = legacy && sessions().find(s => s.source === 'web' && s.chat_id === legacy);
      await select(saved || (prior && prior.session) || sessions()[0]?.session || '');
      connect();
    } catch (e) { setError((e as Error).message); setOnline(false); }
  };
  const timer = setInterval(() => { connect(); void refresh(); }, 4000);
  const focus = () => void refresh();
  window.addEventListener('focus', focus);
  onCleanup(() => { disposed = true; clearInterval(timer); window.removeEventListener('focus', focus); socket?.close(); });
  return { sessions, active, source, more, online, error, thread, session, threads, start, select, fresh, send, rename, remove, load, refresh,
    recover: () => send(false, '/retry'),
    loadMore: () => list(true),
    filter: async (value: string) => { setSource(value); await list(); },
    draft: (value: string) => setThreads(active(), 'draft', value),
    files: (id: string, values: string[]) => { ensure(id); setThreads(id, 'files', values); },
    scroll: (value: number) => setThreads(active(), 'scroll', value),
  };
}
