import { createSignal, For, onCleanup, onMount, Show, type Component, type JSX } from 'solid-js';
import { fetchChatHelp, type ChatHelpPeriod, type ChatHelpSnapshot } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { ScheduleSettings } from '../components/ScheduleSettings';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { currentLocale, t, tf } from '../i18n';

const Metric: Component<{ label: string; children: JSX.Element }> = (props) => (
  <div class="min-w-0 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] px-3 py-2.5">
    <dt class="text-xs text-[var(--color-text-muted)]">{props.label}</dt>
    <dd class="m-0 mt-1 break-words text-sm tabular-nums">{props.children}</dd>
  </div>
);

function number(value: number) {
  return new Intl.NumberFormat(currentLocale()).format(value);
}

function memory(total: number, free: number) {
  return number(Math.floor((total - free) / 1024)) + ' / ' + number(Math.floor(total / 1024)) + ' KiB';
}

const TokenCount: Component<{ period: ChatHelpPeriod; available: boolean }> = (props) => (
  <>
    {props.available && (props.period.reported || !props.period.unreported)
      ? number(props.period.tokens)
      : t('commandUnknown')}
    <Show when={props.period.unreported}>
      <span class="mt-1 block text-xs text-[var(--color-text-muted)]">
        {tf('commandUnreported', { count: number(props.period.unreported) })}
      </span>
    </Show>
  </>
);

const CacheHit: Component<{ period: ChatHelpPeriod }> = (props) => (
  <Show when={props.period.cache_input} fallback={t('commandUnknown')}>
    {((100 * (props.period.cache_hit ?? 0)) / props.period.cache_input!).toFixed(1)}%
    {' · '}{number(props.period.cache_hit ?? 0)} / {number(props.period.cache_input!)} token
  </Show>
);

export const ChatHelpPage: Component = () => {
  const [snapshot, setSnapshot] = createSignal<ChatHelpSnapshot>();
  const [loading, setLoading] = createSignal(true);
  const [error, setError] = createSignal('');
  let request: AbortController | undefined;

  const refresh = async () => {
    request?.abort();
    const controller = new AbortController();
    request = controller;
    setLoading(true);
    setError('');
    try {
      const data = await fetchChatHelp(controller.signal);
      if (!controller.signal.aborted) setSnapshot(data);
    } catch (err) {
      if (!controller.signal.aborted) setError((err as Error).message);
    } finally {
      if (!controller.signal.aborted) setLoading(false);
    }
  };
  onMount(() => void refresh());
  onCleanup(() => request?.abort());

  return (
    <TabShell>
      <PageHeader
        title={t('navCommands')}
        description={t('commandPageDescription')}
        actions={
          <Button variant="secondary" size="sm" disabled={loading()} onClick={() => void refresh()}>
            {t('commandRefresh')}
          </Button>
        }
      />
      <div class="space-y-4 px-5 py-4">
        <Banner kind="error" message={error()} />
        <Show when={snapshot()} fallback={<Show when={loading()}>{t('statusLoading')}</Show>}>
          {(data) => (
            <>
              <dl class="m-0 grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
                <Metric label={t('commandDate')}>{data().date || t('commandUnknown')}</Metric>
                <Metric label={t('commandCpu')}>
                  {data().cpu_percent === null ? t('commandUnknown') : data().cpu_percent + '%'}
                </Metric>
                <Metric label={t('commandRam')}>
                  {memory(data().internal_total, data().internal_free)}
                </Metric>
                <Show when={data().psram_total > 0}>
                  <Metric label={t('commandPsram')}>
                    {memory(data().psram_total, data().psram_free)}
                  </Metric>
                </Show>
                <Metric label={t('commandToday')}>
                  <TokenCount period={data().today} available={data().usage_available} />
                </Metric>
                <Metric label={t('commandMonth')}>
                  <TokenCount period={data().month} available={data().usage_available} />
                </Metric>
                <Metric label={currentLocale() === 'zh-cn' ? '今日输入缓存命中率' : 'Input cache hit rate today'}><CacheHit period={data().today} /></Metric>
                <Metric label={currentLocale() === 'zh-cn' ? '本月输入缓存命中率' : 'Input cache hit rate this month'}><CacheHit period={data().month} /></Metric>
                <Metric label={t('commandAddress')}>
                  <Show when={data().ip} fallback={t('commandUnknown')}>
                    {(ip) => <a class="underline" href={'http://' + ip() + '/'}>{'http://' + ip() + '/'}</a>}
                  </Show>
                </Metric>
              </dl>
              <p class="m-0 text-xs text-[var(--color-text-muted)]">
                {t('commandUsageNote')}
                <Show when={data().usage_available && !data().usage_durable}>
                  {' '}{t('commandVolatile')}
                </Show>
              </p>
              <Show when={data().ready} fallback={<Banner message={t('commandPending')} />}>
                <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] p-0">
                  <For each={data().commands}>
                    {(item) => (
                      <li class="flex flex-wrap items-baseline gap-x-3 gap-y-1 py-2.5">
                        <code class="break-all text-sm text-[var(--color-text-primary)]">{item.command}</code>
                        <span class="text-xs text-[var(--color-text-muted)]">
                          {currentLocale() === 'zh-cn' ? item.description_zh || item.description : item.description}
                          {item.channel && ' (' + item.channel + ')'}
                        </span>
                      </li>
                    )}
                  </For>
                </ul>
              </Show>
            </>
          )}
        </Show>
        <ScheduleSettings />
      </div>
    </TabShell>
  );
};
