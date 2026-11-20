import { For, Show, createSignal, onCleanup, onMount } from 'solid-js';
import { fetchMediaModels, saveDeviceSetting, type MediaKind, type MediaModel } from '../api/client';
import { currentLocale, t } from '../i18n';
import { Banner } from './ui/Banner';
import { Button } from './ui/Button';
import { StaticConfigBlock } from './ui/ConfigBlocks';
import { SelectInput, TextInput } from './ui/FormField';
import { Switch } from './ui/Switch';

const blank = (): MediaModel => ({ name: '', model: '', url: '', kind: 'image', enabled: false });
const options = [
  ['model_field', '模型字段', 'Model field', 'model'],
  ['input_field', '输入字段', 'Input field', 'prompt / input / file'],
  ['result_path', '结果字段路径', 'Result path', 'data.0.url / url / text'],
  ['auth_header', '鉴权请求头', 'Auth header', 'Authorization'],
  ['auth_prefix', '密钥前缀', 'Key prefix', 'Bearer '],
  ['job_path', '任务 ID 路径', 'Job ID path', 'job_id'],
  ['poll_url', '查询任务的 URL', 'Job polling URL', 'https://example.com/jobs/{id}'],
  ['status_path', '任务状态路径', 'Job status path', 'status'],
  ['success_value', '任务成功状态值', 'Success status', 'completed'],
  ['failure_value', '任务失败状态值', 'Failure status', 'failed'],
] as const;

export const CloudMediaSettings = () => {
  const local = (zh: string, en: string) => currentLocale() === 'zh-cn' ? zh : en;
  const label = (kind: MediaKind) => ({ image: local('生图', 'Image generation'), tts: local('语音合成', 'Speech synthesis'), stt: local('语音转写', 'Transcription') })[kind];
  const [models, setModels] = createSignal<MediaModel[]>();
  const [editing, setEditing] = createSignal<MediaModel>();
  const [original, setOriginal] = createSignal('');
  const [key, setKey] = createSignal('');
  const [parameters, setParameters] = createSignal('');
  const [error, setError] = createSignal('');
  const [busy, setBusy] = createSignal(false);
  let editor: HTMLFormElement | undefined;
  const refresh = async (quiet = false) => {
    try {
      const next = await fetchMediaModels();
      if (JSON.stringify(next) !== JSON.stringify(models())) setModels(next);
      if (!quiet) setError('');
    }
    catch (e) { if (!quiet) setError((e as Error).message); }
  };
  onMount(() => {
    void refresh();
    const focus = () => { if (!busy() && document.visibilityState === 'visible') void refresh(true); };
    const timer = window.setInterval(focus, 8000);
    window.addEventListener('focus', focus);
    onCleanup(() => { window.clearInterval(timer); window.removeEventListener('focus', focus); });
  });
  const edit = (model?: MediaModel) => {
    setEditing(model ? { ...model } : blank());
    setOriginal(model?.name || '');
    setKey('');
    setParameters(model?.parameters ? JSON.stringify(model.parameters) : '');
    setError('');
    requestAnimationFrame(() => editor?.scrollIntoView({ block: 'nearest' }));
  };
  const patch = (value: Partial<MediaModel>) => setEditing(prev => prev ? { ...prev, ...value } : prev);
  const mutate = async (data: object) => {
    setError(''); setBusy(true);
    try {
      await saveDeviceSetting('/api/media-models', data);
      setEditing(undefined); setKey('');
      await refresh();
    } catch (e) { setError((e as Error).message); }
    finally { setBusy(false); }
  };
  const save = (event: SubmitEvent) => {
    event.preventDefault();
    if (busy() || !editing()) return;
    const profile = { ...editing()!, name: editing()!.name.trim(), url: editing()!.url.trim(), model: editing()!.model.trim() };
    delete profile.api_key_set;
    try {
      profile.parameters = parameters().trim() ? JSON.parse(parameters()) : undefined;
      if (profile.parameters !== undefined && (!profile.parameters || Array.isArray(profile.parameters) || typeof profile.parameters !== 'object')) throw new Error();
    } catch {
      setError(local('附加参数须为 JSON 对象。', 'Extra parameters must be a JSON object.'));
      return;
    }
    void mutate({ action: 'save', profile: { ...profile, ...(key() ? { api_key: key() } : {}) } });
  };

  return (
    <StaticConfigBlock title={local('云端图片与语音模型', 'Cloud image and speech models')} class="border-t border-[var(--color-border-subtle)]">
      <div class="space-y-3 pt-2">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
          <p class="m-0 max-w-xl text-[0.8rem] leading-relaxed text-[var(--color-text-secondary)]">{local(
            '把生图接口地址、模型 ID 和文档发给机器人创建草稿，再在这里填写密钥并启用。',
            'Send the image API URL, model ID, and docs in chat to create a draft. Enter the key and enable it here.',
          )}</p>
          <div class="flex flex-wrap gap-2 sm:shrink-0">
            <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => void refresh()}>{t('commandRefresh')}</Button>
            <Button size="sm" class="min-h-11 sm:min-h-0" variant="primary" disabled={busy()} onClick={() => edit()}>{local('添加模型', 'Add model')}</Button>
          </div>
        </div>
        <Show when={!editing()}>
          <Banner kind="error" message={error()} class="wrap-anywhere" />
        </Show>
        <Show when={models()?.length} fallback={
          <Show when={models()?.length === 0 && !editing()}>
            <p class="m-0 rounded-lg border border-[var(--color-border-subtle)] px-3 py-2.5 text-sm text-[var(--color-text-secondary)]">{local('还没有云端媒体模型。', 'No cloud media models yet.')}</p>
          </Show>
        }>
          <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] rounded-lg border border-[var(--color-border-subtle)] p-0">
            <For each={models()}>{model => (
              <li class="flex flex-col gap-2 px-3 py-3 sm:flex-row sm:items-center sm:justify-between">
                <div class="min-w-0">
                  <div class="wrap-anywhere text-sm font-medium"><span translate="no">{model.name}</span> · {label(model.kind)}</div>
                  <div class="mt-1 wrap-anywhere text-xs text-[var(--color-text-secondary)]" translate="no">{model.model}</div>
                  <div class="mt-1 text-xs text-[var(--color-text-secondary)]">
                    {model.enabled ? local('已启用', 'Enabled') : local('草稿 / 已停用', 'Draft / disabled')} · {model.api_key_set ? local('已设置密钥', 'Key saved') : local('未设置密钥', 'No key')}
                  </div>
                </div>
                <div class="flex flex-wrap gap-2 sm:shrink-0">
                  <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => edit(model)}>{local('编辑', 'Edit')}</Button>
                  <Button size="sm" class="min-h-11 sm:min-h-0" variant="danger-ghost" disabled={busy()} onClick={() => {
                    if (window.confirm(local('删除此媒体模型？', 'Delete this media model?'))) void mutate({ action: 'remove', name: model.name });
                  }}>{local('删除', 'Delete')}</Button>
                </div>
              </li>
            )}</For>
          </ul>
        </Show>
        <Show when={editing()}>
          <form ref={editor} class="space-y-3 rounded-lg border border-[var(--color-border-subtle)] p-3 sm:p-4" onSubmit={save}>
            <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <TextInput name="name" spellcheck={false} label={local('配置名称', 'Profile name')} required pattern="(?:[A-Za-z0-9_]|-)+" maxLength={48}
                readOnly={!!original()} value={editing()?.name} onInput={e => patch({ name: e.currentTarget.value })} />
              <SelectInput name="kind" label={local('类型', 'Type')}
                disabled={!!original()} value={editing()?.kind} onChange={e => patch({ kind: e.currentTarget.value as MediaKind })}>
                <For each={['image', 'tts', 'stt'] as const}>{kind => <option value={kind}>{label(kind)}</option>}</For>
              </SelectInput>
              <TextInput name="url" spellcheck={false} label="API URL" type="url" required maxLength={512} value={editing()?.url} onInput={e => patch({ url: e.currentTarget.value })} />
              <TextInput name="model" spellcheck={false} label={local('模型 ID', 'Model ID')} required maxLength={128} value={editing()?.model} onInput={e => patch({ model: e.currentTarget.value })} />
              <TextInput name="api_key" label={local('媒体 API 密钥', 'Media API key')} type="password" autocomplete="new-password"
                placeholder={editing()?.api_key_set ? local('留空保留原密钥', 'Leave blank to keep the key') : ''}
                value={key()} onInput={e => setKey(e.currentTarget.value)} />
              <Switch class="min-h-10 w-fit self-end" label={local('启用该模型', 'Enable this model')}
                checked={!!editing()?.enabled} onChange={enabled => patch({ enabled })} />
            </div>
            <details class="border-t border-[var(--color-border-subtle)] text-sm">
              <summary class="cursor-pointer py-2.5">{local('接口字段与异步任务', 'API fields and async jobs')}</summary>
              <div class="grid grid-cols-1 gap-3 pt-1 sm:grid-cols-2">
                <For each={options}>{([field, zh, en, placeholder], index) => <Show when={index() < 5 || editing()?.kind !== 'stt'}>
                  <TextInput name={field} spellcheck={false} label={local(zh, en)} placeholder={placeholder} value={editing()?.[field]} onInput={e => patch({ [field]: e.currentTarget.value })} />
                </Show>}</For>
                <TextInput name="parameters" spellcheck={false} label={local('附加参数 JSON', 'Extra JSON parameters')} maxLength={1024} value={parameters()} placeholder='{"size":"1024x1024"}' onInput={e => { setParameters(e.currentTarget.value); setError(''); }} />
              </div>
            </details>
            <Banner kind="error" message={error()} class="wrap-anywhere" />
            <div class="flex flex-wrap gap-2">
              <Button type="submit" variant="primary" size="sm" class="min-h-11 sm:min-h-0" disabled={busy()}>{local('保存媒体模型', 'Save media model')}</Button>
              <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => { setEditing(undefined); setKey(''); }}>{t('chatCancel')}</Button>
            </div>
          </form>
        </Show>
      </div>
    </StaticConfigBlock>
  );
};
