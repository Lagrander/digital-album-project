/* =========================================================================
   云端相册核心交互脚本 (Premium UX & AJAX)
   ========================================================================= */

// 1. 全局暗黑模式切换
function initTheme() {
    const toggleBtn = document.getElementById('theme-toggle');
    if (!toggleBtn) return;
    
    // 从 localStorage 读取偏好，默认走暗黑模式，因为暗黑更酷
    let currentTheme = localStorage.getItem('theme') || 'dark';
    document.documentElement.setAttribute('data-theme', currentTheme);
    toggleBtn.textContent = currentTheme === 'dark' ? '☀️' : '🌙';

    toggleBtn.addEventListener('click', () => {
        currentTheme = currentTheme === 'dark' ? 'light' : 'dark';
        document.documentElement.setAttribute('data-theme', currentTheme);
        localStorage.setItem('theme', currentTheme);
        toggleBtn.textContent = currentTheme === 'dark' ? '☀️' : '🌙';
    });
}

// 2. Review 页面交互
function initReviewPage() {
    // A. 垃圾桶隐藏删除功能
    document.querySelectorAll('.delete-btn').forEach(btn => {
        btn.addEventListener('click', async (e) => {
            e.stopPropagation(); // 阻止触发全屏详情
            if (!confirm('确定要将此照片从相册中隐藏/删除吗？\n(物理文件保留，但相册不再展示)')) return;
            const path = btn.dataset.path;
            const res = await fetch('/api/photos/hide', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path })
            });
            const data = await res.json();
            if (data.ok) {
                // 优雅的淡出动画移除卡片
                const card = btn.closest('.photo-card');
                card.style.transform = 'scale(0.8)';
                card.style.opacity = '0';
                setTimeout(() => card.remove(), 300);
            }
        });
    });

    // B. 一句话旁白快捷编辑 (In-place Edit)
    document.querySelectorAll('.edit-icon').forEach(icon => {
        icon.addEventListener('click', (e) => {
            e.stopPropagation();
            const container = icon.closest('.side-caption');
            const path = icon.dataset.path;
            const oldTextNode = container.querySelector('span');
            const oldText = oldTextNode.textContent.trim();
            
            // 变成输入框
            const input = document.createElement('input');
            input.type = 'text';
            input.value = oldText;
            input.className = 'inline-edit-input';
            
            container.innerHTML = '';
            container.appendChild(input);
            input.focus();

            const save = async () => {
                const newText = input.value.trim();
                container.innerHTML = `<span>${newText}</span><i class="edit-icon" data-path="${path}">✏️</i>`;
                if (newText !== oldText) {
                    await fetch('/api/photos/update_side_caption', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ path, side_caption: newText })
                    });
                }
                // 重新绑定事件
                initReviewPage();
            };

            input.addEventListener('blur', save);
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') {
                    e.preventDefault();
                    input.blur();
                }
            });
        });
    });

    // C. 全屏模态框展示
    const modal = document.getElementById('photo-modal');
    if (!modal) return;
    
    document.querySelectorAll('.img-wrap').forEach(wrap => {
        wrap.addEventListener('click', () => {
            const card = wrap.closest('.photo-card');
            const data = card.dataset;
            
            document.getElementById('m-img').src = data.imgUri;
            document.getElementById('m-caption').textContent = data.caption || '无长描述';
            document.getElementById('m-reason').textContent = data.reason || '无';
            document.getElementById('m-type').textContent = data.type || '无分类';
            
            // 构建 EXIF 信息
            let exifHtml = `
                <div class="exif-item">拍摄时间<span class="val">${data.datetime || '未知'}</span></div>
                <div class="exif-item">设备<span class="val">${data.make || ''} ${data.model || '未知'}</span></div>
                <div class="exif-item">内存得分<span class="val" style="color:var(--accent)">${parseFloat(data.memory).toFixed(1)}</span></div>
                <div class="exif-item">美观得分<span class="val" style="color:var(--success)">${parseFloat(data.beauty).toFixed(1)}</span></div>
            `;
            if (data.city) {
                const lat = parseFloat(data.lat).toFixed(4);
                const lon = parseFloat(data.lon).toFixed(4);
                exifHtml += `<div class="exif-item">拍摄地<span class="val">${data.city} <a href="https://uri.amap.com/marker?position=${lon},${lat}&name=${data.city}" target="_blank" style="color:#3b82f6;font-size:0.8rem;margin-left:4px">🌍地图</a></span></div>`;
            }
            document.getElementById('m-exif').innerHTML = exifHtml;
            
            // 标签编辑逻辑
            const tagsList = document.getElementById('m-tags');
            tagsList.innerHTML = '';
            const existingTags = data.tags ? data.tags.split(',').filter(t=>t.trim()) : [];
            existingTags.forEach(t => {
                tagsList.innerHTML += `<span class="tag-badge">${t}</span>`;
            });
            tagsList.innerHTML += `<button id="add-tag-btn" style="border:none;background:none;color:var(--accent);cursor:pointer;font-size:0.85rem">+ 添加标签</button>`;
            
            modal.classList.add('active');

            // 绑定添加标签事件
            document.getElementById('add-tag-btn').onclick = async () => {
                const newTag = prompt('请输入新标签名称：');
                if (newTag && newTag.trim()) {
                    existingTags.push(newTag.trim());
                    const finalTags = existingTags.join(',');
                    await fetch('/api/photos/update_tags', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ path: data.path, tags: finalTags })
                    });
                    // 更新视图
                    card.dataset.tags = finalTags;
                    modal.classList.remove('active');
                    setTimeout(() => document.location.reload(), 300); // 简单粗暴重载以刷新左侧过滤条件
                }
            };
        });
    });

    document.querySelector('.modal-close').addEventListener('click', () => {
        modal.classList.remove('active');
        document.getElementById('m-img').src = '';
    });
}

// 3. 极客风 Premium 状态提示模态框 (自研 HSL & 玻璃拟态特效)
function showPremiumModal({ type = 'success', title = '', message = '', buttonText = '确定', onClose = null }) {
    // 移除可能存在的旧模态框
    const oldOverlay = document.getElementById('premium-state-overlay');
    if (oldOverlay) oldOverlay.remove();

    // 创建模糊遮罩蒙版
    const overlay = document.createElement('div');
    overlay.id = 'premium-state-overlay';
    overlay.className = 'premium-modal-overlay';

    let iconEmoji = '🎉';
    let iconClass = 'bounce';
    if (type === 'loading') {
        iconEmoji = '⏳';
        iconClass = 'loading';
    } else if (type === 'error') {
        iconEmoji = '❌';
        iconClass = 'shake';
    }

    // 动态生成高级拟态卡片 HTML
    const contentHtml = `
        <div class="premium-modal-card ${type}">
            <div class="premium-modal-icon ${iconClass}">${iconEmoji}</div>
            <h3 class="premium-modal-title">${title}</h3>
            <p class="premium-modal-text">${message}</p>
            ${type !== 'loading' ? `
                <button class="btn btn-primary premium-modal-btn" id="premium-modal-confirm">${buttonText}</button>
            ` : ''}
        </div>
    `;
    overlay.innerHTML = contentHtml;
    document.body.appendChild(overlay);

    // 触发过渡入场动画
    overlay.offsetHeight;
    overlay.classList.add('active');

    const closeModal = () => {
        overlay.classList.remove('active');
        setTimeout(() => {
            overlay.remove();
            if (onClose) onClose();
        }, 300);
    };

    // 绑定非加载状态下的关闭行为
    if (type !== 'loading') {
        const confirmBtn = overlay.querySelector('#premium-modal-confirm');
        confirmBtn.addEventListener('click', closeModal);
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) closeModal();
        });
    }

    return {
        close: closeModal
    };
}

// 4. Upload 页面：3D ESP32 模拟器联动与 AJAX 惊喜发送
function initUploadPage() {
    const fileInput = document.getElementById('file-input');
    const msgInput = document.getElementById('message');
    const nameInput = document.getElementById('name');
    const uploadForm = document.getElementById('upload-form');
    if (!fileInput) return;

    // 动态同步更新 3D 模拟器顶栏的当前真实时间
    const simTimeEl = document.getElementById('sim-time');
    if(simTimeEl) {
        const updateSimTime = () => {
            const d = new Date();
            const hh = String(d.getHours()).padStart(2, '0');
            const mm = String(d.getMinutes()).padStart(2, '0');
            simTimeEl.textContent = `${hh}:${mm}`;
        };
        updateSimTime();
        setInterval(updateSimTime, 60000);
    }

    const defaultBgSrc = "https://via.placeholder.com/480x800/1a1a1a/1a1a1a";
    const defaultCaption = "等待输入照片旁白...";

    // 同步表单悄悄话到 3D 屏幕
    msgInput.addEventListener('input', () => {
        const val = msgInput.value.trim() || defaultCaption;
        document.getElementById('sim-caption').textContent = val;
    });
    
    // 本地图片选择后双重绑定预览
    fileInput.addEventListener('change', (e) => {
        if (e.target.files.length > 0) {
            const file = e.target.files[0];
            const reader = new FileReader();
            reader.onload = (ev) => {
                document.getElementById('sim-bg').src = ev.target.result;
                document.getElementById('sim-fg').src = ev.target.result;
                document.querySelector('.upload-zone').style.borderColor = 'var(--success)';
            };
            reader.readAsDataURL(file);
        }
    });

    // 拦截表单提交，执行 AJAX 异步无跳转投递
    if(uploadForm) {
        uploadForm.addEventListener('submit', async (e) => {
            e.preventDefault();

            // 1. 弹出 Premium Loading 动画模态框
            const loadingModal = showPremiumModal({
                type: 'loading',
                title: '正在传送今日份的照片...',
                message: '正在为您打包照片数据，进行高斯模糊底板合成、真机宽度适配裁切以及 RGB565 终端流打包，这可能需要数秒时间，请耐心等待~'
            });

            try {
                const formData = new FormData(uploadForm);
                const res = await fetch('/upload/send', {
                    method: 'POST',
                    body: formData
                });
                const data = await res.json();

                // 2. 至少保留 800ms 的 Loading，以保障动效过渡自然柔和
                setTimeout(() => {
                    loadingModal.close();

                    setTimeout(() => {
                        if(res.ok && data.ok) {
                            // 传送成功
                            showPremiumModal({
                                type: 'success',
                                title: '传送成功！心意已送达',
                                message: '您打包的惊喜已完美送达云端排队队列。当远方的电子相册同步时，即可瞬间点亮屏幕并浮现您的悄悄话~',
                                buttonText: '确定',
                                onClose: () => {
                                    // A. 重置表单输入
                                    uploadForm.reset();

                                    // B. 复原 3D ESP32 模拟器状态
                                    document.getElementById('sim-bg').src = defaultBgSrc;
                                    document.getElementById('sim-fg').src = "";
                                    document.getElementById('sim-caption').textContent = defaultCaption;
                                    document.querySelector('.upload-zone').style.borderColor = '';

                                    // C. 重新设定目标日期为当前日期
                                    const todayStr = new Date().toISOString().split('T')[0];
                                    const targetDateInput = document.getElementById('target-date');
                                    if(targetDateInput) targetDateInput.value = todayStr;

                                    // D. 触发局部无刷新重载历史记录列表
                                    if(typeof window.loadUploadHistory === 'function') {
                                        window.loadUploadHistory();
                                    }
                                }
                            });
                        } else {
                            // 后端业务校验失败
                            showPremiumModal({
                                type: 'error',
                                title: '传送失败，请检查配置',
                                message: data.error || '传送失败，后端未能正确处理请求，请稍后重试。',
                                buttonText: '返回修改'
                            });
                        }
                    }, 100);
                }, 800);

            } catch(err) {
                console.error('AJAX 发送异常:', err);
                loadingModal.close();
                setTimeout(() => {
                    showPremiumModal({
                        type: 'error',
                        title: '网络连接异常',
                        message: '无法与服务器建立稳定的网络连接，请检查您的局域网环境并重试。',
                        buttonText: '重新尝试'
                    });
                }, 300);
            }
        });
    }

    // 历史队列删除 (AJAX 局部更新模式)
    document.querySelectorAll('.del-upload-btn').forEach(btn => {
        btn.addEventListener('click', async () => {
            if (!confirm('取消此排队惊喜？')) return;
            const res = await fetch('/api/upload/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ id: btn.dataset.id })
            });
            const data = await res.json();
            if (data.ok) {
                if (typeof window.loadUploadHistory === 'function') {
                    window.loadUploadHistory();
                } else {
                    document.location.reload();
                }
            }
        });
    });
}


// 4. Device 专属面板：极客轮询与远程控制
function initDevicePage() {
    if (!document.getElementById('device-dashboard')) return;

    const pollStatus = async () => {
        try {
            const res = await fetch('/api/device/status');
            const data = await res.json();
            
            const circle = document.getElementById('status-circle');
            const lbl = document.getElementById('status-lbl');
            
            if (data.online) {
                circle.className = 'status-circle online';
                lbl.textContent = '设备在线 (Online)';
                document.getElementById('val-fps').textContent = parseFloat(data.fps).toFixed(1);
                document.getElementById('val-mem').textContent = (data.free_mem / 1024).toFixed(1) + ' KB';
                
                // 香薰状态同步
                const channels = data.aroma_channels || [0,0,0];
                for(let i=0; i<3; i++) {
                    const card = document.getElementById(`aroma-${i}`);
                    if (channels[i] === 1) card.classList.add('active');
                    else card.classList.remove('active');
                }
            } else {
                circle.className = 'status-circle offline';
                lbl.textContent = '设备离线 (Offline)';
                document.getElementById('val-fps').textContent = '--';
                document.getElementById('val-mem').textContent = '--';
                for(let i=0; i<3; i++) document.getElementById(`aroma-${i}`).classList.remove('active');
            }
        } catch(e) {
            console.error('Device poll error', e);
        }
    };

    // 每 3 秒心跳轮询
    pollStatus();
    setInterval(pollStatus, 3000);

    // 强行切图
    document.getElementById('btn-switch').addEventListener('click', async () => {
        const btn = document.getElementById('btn-switch');
        btn.disabled = true;
        btn.textContent = '指令下发中...';
        await fetch('/api/device/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ cmd: 'switch_photo' })
        });
        setTimeout(() => {
            btn.disabled = false;
            btn.textContent = '立即强行切换照片';
        }, 1500);
    });

    // 香薰点击遥控
    for(let i=0; i<3; i++) {
        document.getElementById(`aroma-${i}`).addEventListener('click', async (e) => {
            const card = e.currentTarget;
            const currentState = card.classList.contains('active') ? 1 : 0;
            const targetState = currentState === 1 ? 0 : 1;
            
            await fetch('/api/device/control', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ cmd: 'toggle_aroma', channel: i, state: targetState })
            });
            // 乐观 UI 更新
            if(targetState) card.classList.add('active');
            else card.classList.remove('active');
        });
    }
}

// 5. Dialogs 页面：智能助手聊天（已由 dialogs.html 内联 SSE 脚本接管，此处留空）
function initDialogsPage() {
    // 逻辑已迁移到 dialogs.html 内联 <script> 中，通过 SSE 实时接收 AI 回复
}

// 全局初始化
document.addEventListener('DOMContentLoaded', () => {
    initTheme();
    initReviewPage();
    initUploadPage();
    initDevicePage();
    initDialogsPage();
});
