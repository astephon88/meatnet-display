{{- define "meatnet-app.fullname" -}}
{{- if contains .Chart.Name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name .Chart.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "meatnet-app.labels" -}}
app.kubernetes.io/name: {{ .Chart.Name }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
{{- end -}}

{{- define "meatnet-app.selectorLabels" -}}
app.kubernetes.io/name: {{ .Chart.Name }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}

{{- define "meatnet-app.secretName" -}}
{{- if .Values.splitKey.existingSecret -}}
{{- .Values.splitKey.existingSecret -}}
{{- else if .Values.splitKey.value -}}
{{- include "meatnet-app.fullname" . }}-key
{{- else -}}
{{- fail "set splitKey.existingSecret or splitKey.value" -}}
{{- end -}}
{{- end -}}
