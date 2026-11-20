import { For, Show, createSignal, onMount } from 'solid-js';
import { fetchDeviceSchedules, fetchWebimSessions, saveDeviceSetting, type DeviceSchedule, type WebImSession } from '../api/client';
import { currentLocale, t } from '../i18n';
import { Banner } from './ui/Banner';
import { Button } from './ui/Button';
import { TextInput } from './ui/FormField';

export const ScheduleSettings = () => {
  const local = (zh: string, en: string) => currentLocale() === 'zh-cn' ? zh : en;
  const [schedules, setSchedules] = createSignal<DeviceSchedule[]>();
  const [sessions, setSessions] = createSignal<WebImSession[]>([]);
  const [error, setError] = createSignal('');
  const [busy, setBusy] = createSignal(false);
  const [target, setTarget] = createSignal('');
  const [kind, setKind] = createSignal('once');
  const [when, setWhen] = createSignal('');
  const [message, setMessage] = createSignal('');
  const refresh = async () => {
    setError('');
    try {
      const list = await fetchDeviceSchedules();
      if (!Array.isArray(list)) return;
      setSchedules(list);
      const page = await fetchWebimSessions();
      setSessions(page.items.filter(s => s.can_send && !!s.reply_channel));
    } catch (e) { setError((e as Error).message); }
  };
  onMount(() => void refresh());
  const save = async (data: object, clear = false) => {
    setBusy(true); setError('');
    try {
      await saveDeviceSetting('/api/schedules', data);
      if (clear) { setWhen(''); setMessage(''); }
      await refresh();
    } catch (e) { setError((e as Error).message); }
    finally { setBusy(false); }
  };
  const submit = (event: SubmitEvent) => {
    event.preventDefault();
    const session = sessions().find(s => s.session === target());
    if (busy() || !session) return;
    const [hour, minute] = when().split(':').map(Number);
    const time = kind() === 'once' ? { start_at_ms: new Date(when()).getTime() } :
      kind() === 'interval' ? { minutes: Number(when()) } : { cron_expr: `${minute} ${hour} * * *` };
    void save({ action: 'add', kind: kind(), ...time, message: message().trim(),
      channel: session.reply_channel, chat_id: session.reply_channel === 'web' ? session.session : session.chat_id }, true);
  };
  const format = (time: number) => time > 0 ? new Date(time).toLocaleString(currentLocale()) : '—';
  const selectClass = 'rounded border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-3 py-2 text-sm';
  const status = (value: string) => ({
    scheduled: local('等待发送', 'Scheduled'), completed: local('已完成', 'Completed'),
    running: local('运行中', 'Running'), paused: local('已暂停', 'Paused'),
    disabled: local('已停用', 'Disabled'), error: local('出错', 'Error'),
  })[value] ?? value;
  return (
    <section class="space-y-3 border-t border-[var(--color-border-subtle)] pt-4">
      <div class="flex items-center justify-between gap-2">
        <h2 class="m-0 text-base font-semibold">{local('定时提醒', 'Scheduled reminders')}</h2>
        <Button size="sm" variant="secondary" disabled={busy()} onClick={() => void refresh()}>{t('commandRefresh')}</Button>
      </div>
      <Banner kind="error" message={error()} />
      <Show when={schedules()}>
        <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] p-0">
          <For each={schedules()}>{item => (
            <li class="flex flex-wrap items-center gap-2 py-2">
              <div class="min-w-0 flex-1">
                <div class="break-words text-sm">{item.text || item.id}</div>
                <div class="text-xs text-[var(--color-text-muted)]">{item.source_channel} · {status(item.status)} · {local('下次', 'Next')} {format(item.next_fire_ms)}</div>
              </div>
              <Button size="sm" disabled={busy()} onClick={() => void save({ action: 'enable', id: item.id, enabled: !item.enabled })}>
                {item.enabled ? local('停用', 'Disable') : local('启用', 'Enable')}
              </Button>
              <Show when={item.id.startsWith('web_reminder_')}>
                <Button size="sm" variant="ghost" disabled={busy()} onClick={() => {
                  if (window.confirm(local('删除这条定时提醒？', 'Delete this reminder?'))) void save({ action: 'remove', id: item.id });
                }}>{local('删除', 'Delete')}</Button>
              </Show>
            </li>
          )}</For>
        </ul>
        <form class="grid items-end gap-3 sm:grid-cols-2" onSubmit={submit}>
          <label class="grid gap-1 text-xs">{local('发送到', 'Send to')}
            <select class={selectClass} required value={target()} onChange={e => setTarget(e.currentTarget.value)}>
              <option value="">{local('选择最近的聊天', 'Choose a recent chat')}</option>
              <For each={sessions()}>{session => <option value={session.session}>{session.reply_channel} · {session.title || session.alias || session.chat_id}</option>}</For>
            </select>
          </label>
          <label class="grid gap-1 text-xs">{local('频率', 'Frequency')}
            <select class={selectClass} value={kind()} onChange={e => { setKind(e.currentTarget.value); setWhen(''); }}>
              <option value="once">{local('一次', 'Once')}</option>
              <option value="interval">{local('固定间隔', 'At an interval')}</option>
              <option value="cron">{local('每天（设备时区）', 'Daily (device time)')}</option>
            </select>
          </label>
          <TextInput label={kind() === 'interval' ? local('间隔分钟数', 'Interval in minutes') : local('发送时间', 'Send at')}
            type={kind() === 'once' ? 'datetime-local' : kind() === 'cron' ? 'time' : 'number'}
            min={kind() === 'interval' ? '1' : undefined} max={kind() === 'interval' ? '525600' : undefined}
            required value={when()} onInput={e => setWhen(e.currentTarget.value)} />
          <TextInput label={local('提醒内容（最多 255 字节）', 'Reminder text (up to 255 bytes)')} required
            value={message()} onInput={e => setMessage(e.currentTarget.value)} />
          <Button type="submit" variant="primary" size="sm" class="justify-self-start"
            disabled={busy() || !target() || !when() || !message().trim() || new TextEncoder().encode(message()).length > 255}>
            {local('添加提醒', 'Add reminder')}
          </Button>
        </form>
      </Show>
    </section>
  );
};
