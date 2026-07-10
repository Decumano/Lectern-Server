// Login/register gate shown in front of the app on the web build. On Tauri
// there's no account system, so this just gets out of the way immediately.
// main.js is loaded dynamically (not via a static <script> tag) so it never
// runs — and never touches workFolderRoot-gated Platform calls — until we
// know either we're on Tauri or the user is authenticated.

(function () {
  var gate = document.getElementById('auth-gate');
  var form = document.getElementById('auth-gate-form');
  var emailInput = document.getElementById('auth-gate-email');
  var passwordInput = document.getElementById('auth-gate-password');
  var errorBox = document.getElementById('auth-gate-error');
  var submitBtn = document.getElementById('auth-gate-submit');
  var toggleLink = document.getElementById('auth-gate-toggle');
  var toggleLabel = document.getElementById('auth-gate-toggle-label');
  var mode = 'login';

  function loadApp() {
    gate.style.display = 'none';
    var script = document.createElement('script');
    script.src = 'main.js';
    document.body.appendChild(script);
  }

  function showError(message) {
    errorBox.textContent = message;
    errorBox.style.display = 'block';
  }

  function setMode(next) {
    mode = next;
    submitBtn.textContent = mode === 'login' ? 'Log in' : 'Create account';
    toggleLabel.textContent = mode === 'login' ? 'No account?' : 'Already have one?';
    toggleLink.textContent = mode === 'login' ? 'Register' : 'Log in';
    errorBox.style.display = 'none';
  }

  toggleLink.addEventListener('click', function (e) {
    e.preventDefault();
    setMode(mode === 'login' ? 'register' : 'login');
  });

  form.addEventListener('submit', function (e) {
    e.preventDefault();
    submitBtn.disabled = true;
    var action = mode === 'login' ? Platform.login : Platform.register;
    action(emailInput.value, passwordInput.value)
      .then(loadApp)
      .catch(function (err) {
        showError(err.message || 'Something went wrong');
        submitBtn.disabled = false;
      });
  });

  if (Platform.isNative) {
    loadApp();
    return;
  }

  Platform.currentUser().then(function (user) {
    if (user) {
      loadApp();
    } else {
      gate.style.display = 'flex';
    }
  });
})();
