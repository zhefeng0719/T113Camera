#!/bin/sh
# 仅提供可选启动入口；审批前不写入 /etc/init.d，不自动后台重启应用。
set -eu
cd "${T113_APP_DIR:-/root/Project/T113Camera}"
export T113_MIC_GAIN=${T113_MIC_GAIN:-8}
export T113_ADC_VOLUME=${T113_ADC_VOLUME:-120}
exec ./t113_camera "$@"
