<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import axios from 'axios'
import { normalizeError, type NormalizedError } from '../../services/errorAdapter'
import { getErrorMessage } from '../../services/messages'

// P1-2 (#278): email-template management. List (key x locale), edit
// (subject / body_html / body_text with the save-time content policy
// enforced server-side), and test-send to a whitelisted recipient.
const { t } = useI18n()

interface TemplateRow {
  template_key: string
  locale: string
  subject: string
  body_html: string
  body_text: string
  updated_at: string
}

const templates = ref<TemplateRow[]>([])
const loading = ref(true)
const showEditModal = ref(false)
const saving = ref(false)
const successMessage = ref('')
const errorMessage = ref<NormalizedError | string | null>(null)
const errorText = computed(() => {
  const e = errorMessage.value
  if (!e) return ''
  return typeof e === 'string' ? e : getErrorMessage(e.code)
})

const editing = ref<TemplateRow | null>(null)
const editForm = ref({ subject: '', body_html: '', body_text: '' })
const testTo = ref('')

function showSuccess(msg: string) {
  successMessage.value = msg
  errorMessage.value = null
  setTimeout(() => { successMessage.value = '' }, 3000)
}
function showError(msg: NormalizedError | string) {
  errorMessage.value = msg
  successMessage.value = ''
  setTimeout(() => { errorMessage.value = null }, 5000)
}

async function fetchTemplates() {
  loading.value = true
  try {
    const resp = await axios.get('/api/admin/email-templates')
    templates.value = resp.data.templates ?? []
  } catch (e) {
    showError(normalizeError(e))
  } finally {
    loading.value = false
  }
}

function openEdit(row: TemplateRow) {
  editing.value = row
  editForm.value = {
    subject: row.subject,
    body_html: row.body_html,
    body_text: row.body_text,
  }
  showEditModal.value = true
}

async function saveTemplate() {
  if (!editing.value) return
  saving.value = true
  try {
    await axios.put(
      `/api/admin/email-templates/${encodeURIComponent(editing.value.template_key)}/${encodeURIComponent(editing.value.locale)}`,
      editForm.value,
    )
    showEditModal.value = false
    showSuccess(t('admin.emailTemplates.saved'))
    await fetchTemplates()
  } catch (e) {
    // Content-policy / quota rejections surface the catalog message.
    showError(normalizeError(e))
  } finally {
    saving.value = false
  }
}

async function testSend(row: TemplateRow) {
  if (!testTo.value) {
    showError(t('admin.emailTemplates.testRecipientRequired'))
    return
  }
  try {
    const resp = await axios.post(
      `/api/admin/email-templates/${encodeURIComponent(row.template_key)}/${encodeURIComponent(row.locale)}/test-send`,
      { to: testTo.value },
    )
    showSuccess(t('admin.emailTemplates.testSent', { subject: resp.data.subject ?? row.subject }))
  } catch (e) {
    showError(normalizeError(e))
  }
}

onMounted(fetchTemplates)
</script>

<template>
  <div class="p-6">
    <h1 class="text-2xl font-bold mb-4">{{ t('admin.emailTemplates.title') }}</h1>
    <p class="text-sm text-base-content/70 mb-4">{{ t('admin.emailTemplates.subtitle') }}</p>

    <div v-if="successMessage" class="alert alert-success mb-4">{{ successMessage }}</div>
    <div v-if="errorText" class="alert alert-error mb-4">{{ errorText }}</div>

    <div class="flex items-center gap-2 mb-4">
      <input
        v-model="testTo"
        type="email"
        class="input input-bordered input-sm w-72"
        :placeholder="t('admin.emailTemplates.testRecipientPlaceholder')"
      />
      <span class="text-xs text-base-content/60">{{ t('admin.emailTemplates.testRecipientHint') }}</span>
    </div>

    <div v-if="loading" class="loading loading-spinner" />
    <div v-else class="overflow-x-auto">
      <table class="table table-zebra">
        <thead>
          <tr>
            <th>{{ t('admin.emailTemplates.key') }}</th>
            <th>{{ t('admin.emailTemplates.locale') }}</th>
            <th>{{ t('admin.emailTemplates.subject') }}</th>
            <th>{{ t('admin.emailTemplates.updatedAt') }}</th>
            <th class="text-right">{{ t('common.actions') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="row in templates" :key="`${row.template_key}/${row.locale}`">
            <td class="font-mono text-sm">{{ row.template_key }}</td>
            <td>{{ row.locale }}</td>
            <td class="max-w-[20rem] truncate">{{ row.subject }}</td>
            <td class="text-sm text-base-content/70">{{ row.updated_at }}</td>
            <td class="text-right space-x-2 whitespace-nowrap">
              <button class="btn btn-xs btn-primary" @click="openEdit(row)">
                {{ t('common.edit') }}
              </button>
              <button class="btn btn-xs btn-outline" @click="testSend(row)">
                {{ t('admin.emailTemplates.testSend') }}
              </button>
            </td>
          </tr>
          <tr v-if="templates.length === 0">
            <td colspan="5" class="text-center text-base-content/60 py-8">
              {{ t('admin.emailTemplates.empty') }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <dialog :class="['modal', { 'modal-open': showEditModal }]">
      <div class="modal-box max-w-3xl">
        <h3 class="font-bold text-lg mb-2">
          {{ t('admin.emailTemplates.editTitle', { key: editing?.template_key ?? '', locale: editing?.locale ?? '' }) }}
        </h3>
        <div class="form-control mb-3">
          <label class="label"><span class="label-text">{{ t('admin.emailTemplates.subject') }}</span></label>
          <input v-model="editForm.subject" type="text" class="input input-bordered w-full" />
        </div>
        <div class="form-control mb-3">
          <label class="label"><span class="label-text">HTML</span></label>
          <textarea
            v-model="editForm.body_html"
            class="textarea textarea-bordered w-full font-mono text-xs"
            rows="10"
          />
        </div>
        <div class="form-control mb-3">
          <label class="label"><span class="label-text">{{ t('admin.emailTemplates.plainText') }}</span></label>
          <textarea
            v-model="editForm.body_text"
            class="textarea textarea-bordered w-full font-mono text-xs"
            rows="6"
          />
        </div>
        <p class="text-xs text-base-content/60 mb-2">{{ t('admin.emailTemplates.policyHint') }}</p>
        <div class="modal-action">
          <button class="btn btn-ghost" @click="showEditModal = false">{{ t('common.cancel') }}</button>
          <button class="btn btn-primary" :disabled="saving" @click="saveTemplate">
            <span v-if="saving" class="loading loading-spinner loading-xs" />
            {{ t('common.save') }}
          </button>
        </div>
      </div>
      <form method="dialog" class="modal-backdrop" @click="showEditModal = false">
        <button>close</button>
      </form>
    </dialog>
  </div>
</template>
