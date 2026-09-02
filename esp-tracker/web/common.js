// Shared client, auth, and formatting helpers for every page in this
// dashboard (tracker.html, attendance.html). One Supabase client, one sign-in
// flow, one place that knows what "how long ago" means — so the two
// dashboards can't drift into inconsistent auth or copy.

const sb = supabase.createClient(window.SUPABASE_URL, window.SUPABASE_ANON_KEY);

const $ = (id) => document.getElementById(id);

const secsAgo = (iso) => Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
function ago(s) {
  if (s == null) return "never";
  if (s < 60) return `${s | 0}s ago`;
  if (s < 3600) return `${(s / 60) | 0} min ago`;
  if (s < 86400) return `${(s / 3600) | 0} hr ago`;
  return `${(s / 86400) | 0} days ago`;
}

// GoTrue's own error strings leak internals ("Database error saving new
// user" is what a race on an already-registered, still-unconfirmed email
// looks like from outside) — translate the ones a parent or staff member
// would otherwise have to screenshot back at us into something actionable.
function friendlySignupError(error) {
  const msg = error?.message || "";
  if (/already registered|user already exists/i.test(msg)) {
    return "That email already has an account. Try signing in, or use \"Check your email\" if you just created it.";
  }
  if (/database error saving new user/i.test(msg)) {
    return "That email may already be registered and awaiting confirmation — check your inbox, or try signing in.";
  }
  return msg || "Couldn't create the account. Please try again.";
}

// Wires the shared #signin form/toggle/signout controls and resolves once a
// session exists — every page calls this first and only builds its own UI
// inside the callback, so there is exactly one auth implementation to get
// right instead of one per page.
async function requireAuth(onReady) {
  let signupMode = false;
  $("signin-toggle")?.addEventListener("click", () => {
    signupMode = !signupMode;
    $("signin-heading").textContent = signupMode ? "Create account" : "Sign in";
    $("signin-submit").textContent = signupMode ? "Create account" : "Sign in";
    $("signin-toggle").textContent = signupMode
      ? "Already have an account? Sign in" : "Need an account? Create one";
    $("password2").hidden = !signupMode;
    $("password2").required = signupMode;
    $("signin-error").textContent = "";
    $("signin-note").hidden = true;
  });

  $("signin-form")?.addEventListener("submit", async (e) => {
    e.preventDefault();
    $("signin-error").textContent = "";
    $("signin-note").hidden = true;

    // Without this, a slow network plus an impatient second click fires two
    // signUp/signIn requests for the same email before the first returns —
    // the second one is what turns into a raw, unhelpful 500 from GoTrue.
    const btn = $("signin-submit");
    if (btn.disabled) return;
    btn.disabled = true;
    const restoreLabel = btn.textContent;

    try {
      if (signupMode) {
        if ($("password").value !== $("password2").value) {
          $("signin-error").textContent = "Passwords don't match.";
          return;
        }
        const { data, error } = await sb.auth.signUp({
          email: $("email").value, password: $("password").value,
        });
        if (error) { $("signin-error").textContent = friendlySignupError(error); return; }
        if (data.session) { location.reload(); return; }
        $("signin-note").hidden = false;
        $("signin-note").textContent = "Check your email to confirm the account, then sign in.";
        return;
      }

      const { error } = await sb.auth.signInWithPassword({
        email: $("email").value, password: $("password").value,
      });
      if (error) { $("signin-error").textContent = error.message; return; }
      location.reload();
    } finally {
      btn.disabled = false;
      btn.textContent = restoreLabel;
    }
  });

  $("signout")?.addEventListener("click", async () => {
    await sb.auth.signOut(); location.reload();
  });

  const { data: { session } } = await sb.auth.getSession();
  if (!session) { $("signin").hidden = false; return; }
  $("signin").hidden = true;
  await onReady();
}
