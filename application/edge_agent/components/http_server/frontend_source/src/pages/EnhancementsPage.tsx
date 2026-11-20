import { ChevronRight, Image, List, Plug, Sparkles } from 'lucide-solid';
import { createSignal, For, Show, type Component } from 'solid-js';
import type { AppConfig, ConfigPatch } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';
import { Banner } from '../components/ui/Banner';
import { TextInput } from '../components/ui/FormField';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Switch } from '../components/ui/Switch';
import { currentLocale, t, tf } from '../i18n';
import { appConfig } from '../state/config';
import { createConfigTab } from '../state/configTab';

const SHORTCUTS = [
  { href: '#mcp', label: 'navMcp', detail: 'enhancementsMcp', icon: Plug },
  { href: '#skill-catalog', label: 'navSkillCatalog', detail: 'enhancementsSkills', icon: Sparkles },
  { href: '#commands', label: 'navCommands', detail: 'enhancementsCommands', icon: List },
  { href: '#webim', label: 'navWebIm', detail: 'enhancementsImages', icon: Image },
] as const;

// The device computes its 60% compaction threshold with a 32-bit multiplication.
const MAX_CONTEXT_WINDOW = Math.floor(0xffffffff / 60);

function readToolProgress(config: Partial<AppConfig>) {
  return config.show_tool_progress === 'true' || config.show_tool_progress === '1';
}

function readAutoStickers(config: Partial<AppConfig>) {
  return config.auto_stickers !== 'false' && config.auto_stickers !== '0';
}

function readStickerRate(config: Partial<AppConfig>) {
  const value = config.sticker_rate ?? '';
  return /^\d{1,3}$/.test(value) && Number(value) <= 100 ? Number(value) : 50;
}

export const EnhancementsPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab({
    tab: 'enhancements',
    fields: ['show_tool_progress', 'auto_stickers', 'sticker_rate', 'llm_context_window'],
    toForm: (config) => ({
      showToolProgress: readToolProgress(config),
      autoStickers: readAutoStickers(config),
      stickerRate: readStickerRate(config),
      contextWindow: config.llm_context_window ?? '',
    }),
    fromForm: (form) => {
      const patch: ConfigPatch = {};
      if (form.showToolProgress !== readToolProgress(appConfig())) {
        patch.show_tool_progress = String(form.showToolProgress);
      }
      if (form.autoStickers !== readAutoStickers(appConfig())) {
        patch.auto_stickers = String(form.autoStickers);
      }
      if (form.stickerRate !== readStickerRate(appConfig())) {
        patch.sticker_rate = String(form.stickerRate);
      }
      if (form.contextWindow.trim() !== (appConfig().llm_context_window ?? '')) {
        patch.llm_context_window = form.contextWindow.trim();
      }
      return patch;
    },
    isDirty: (form, baseline) =>
      form.showToolProgress !== baseline.showToolProgress ||
      form.autoStickers !== baseline.autoStickers ||
      form.stickerRate !== baseline.stickerRate ||
      form.contextWindow.trim() !== baseline.contextWindow,
  });
  const [restartOpen, setRestartOpen] = createSignal(false);
  const [showValidation, setShowValidation] = createSignal(false);
  const contextChanged = () => tab.form.contextWindow.trim() !== (appConfig().llm_context_window ?? '');
  const contextInvalid = () => {
    const value = tab.form.contextWindow.trim();
    return contextChanged() && value !== '' && (!/^\d+$/.test(value) || Number(value) > MAX_CONTEXT_WINDOW);
  };

  const save = async () => {
    if (tab.loading() || tab.saving()) return;
    if (contextInvalid()) {
      setShowValidation(true);
      return;
    }
    const needsRestart = contextChanged();
    await tab.save();
    setShowValidation(false);
    if (needsRestart) setRestartOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navEnhancements')} />
      <ul class="m-0 grid list-none gap-2.5 px-5 py-5 sm:grid-cols-2">
        <For each={SHORTCUTS}>
          {(item) => (
            <li class="min-w-0">
              <a
                href={item.href}
                class="group flex h-full min-h-17 items-center gap-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] px-3.5 py-2.5 transition hover:border-[var(--color-accent-soft)]/55 hover:bg-white/5 focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-[var(--color-accent-soft)]"
              >
                <span class="flex size-9 shrink-0 items-center justify-center rounded-[var(--radius-sm)] bg-[var(--color-accent)]/12 text-[var(--color-accent-soft)]">
                  <item.icon class="size-4.5" aria-hidden="true" />
                </span>
                <span class="flex min-w-0 flex-1 flex-col gap-0.5">
                  <span class="text-sm font-semibold text-[var(--color-text-primary)]">{t(item.label)}</span>
                  <span class="text-xs leading-relaxed text-[var(--color-text-secondary)]">{t(item.detail)}</span>
                </span>
                <ChevronRight
                  class="size-4 shrink-0 text-[var(--color-text-muted)] group-hover:text-[var(--color-text-primary)]"
                  aria-hidden="true"
                />
              </a>
            </li>
          )}
        </For>
      </ul>
      <div class="space-y-4 border-t border-[var(--color-border-subtle)] px-5 py-4">
        <h3 class="m-0 text-sm font-semibold text-[var(--color-text-primary)]">{t('enhancementsChatMemory')}</h3>
        <Show when={tab.error()}>
          <Banner kind="error" message={tab.error() ?? undefined} />
        </Show>
        <Switch
          label={t('showToolProgress')}
          hint={t('showToolProgressHint')}
          checked={tab.form.showToolProgress}
          disabled={tab.loading() || tab.saving()}
          onChange={(checked) => tab.setForm('showToolProgress', checked)}
        />
        <Switch
          label={t('autoStickers')}
          hint={t('autoStickersHint')}
          checked={tab.form.autoStickers}
          disabled={tab.loading() || tab.saving()}
          onChange={(checked) => tab.setForm('autoStickers', checked)}
        />
        <div class="max-w-sm">
          <label for="sticker-rate" class="flex justify-between gap-2">
            {t('stickerRate')}
            <span class="tabular-nums text-[var(--color-text-primary)]" aria-hidden="true">{tab.form.stickerRate}%</span>
          </label>
          <input
            id="sticker-rate"
            name="sticker_rate"
            type="range"
            min={0}
            max={100}
            value={tab.form.stickerRate}
            aria-valuetext={`${tab.form.stickerRate}%`}
            aria-describedby="sticker-rate-hint"
            class="h-11 cursor-pointer border-0 bg-transparent px-0 py-0 accent-[var(--color-accent)] focus-visible:outline-2 focus-visible:outline-[var(--color-accent)] disabled:cursor-not-allowed disabled:opacity-50"
            disabled={!tab.form.autoStickers || tab.loading() || tab.saving()}
            onInput={(event) => tab.setForm('stickerRate', event.currentTarget.valueAsNumber)}
          />
          <p id="sticker-rate-hint" class="m-0 text-xs leading-relaxed text-[var(--color-text-secondary)]">{t('stickerRateHint')}</p>
        </div>
        <TextInput
          label={t('llmContextWindow')}
          placeholder={t('llmContextWindowPlaceholder')}
          hint={t('llmContextWindowHelp')}
          error={
            showValidation() && contextInvalid()
              ? tf('enhancementsContextInvalid', { limit: MAX_CONTEXT_WINDOW.toLocaleString(currentLocale()) })
              : undefined
          }
          aria-invalid={showValidation() && contextInvalid()}
          fieldClass="max-w-xl"
          inputmode="numeric"
          maxLength={15}
          value={tab.form.contextWindow}
          disabled={tab.loading() || tab.saving()}
          onInput={(event) => tab.setForm('contextWindow', event.currentTarget.value)}
        />
      </div>
      <SavePanel
        dirty={!tab.loading() && tab.dirty()}
        saving={tab.saving()}
        onSave={() => void save().catch(() => undefined)}
        onDiscard={() => {
          setShowValidation(false);
          tab.discard();
        }}
      />
      <RestartConfirmModal
        open={restartOpen()}
        subtitle={t('enhancementsContextRestart')}
        onClose={() => setRestartOpen(false)}
        onConfirm={() => {
          setRestartOpen(false);
          props.onRestartRequest();
        }}
      />
    </TabShell>
  );
};
