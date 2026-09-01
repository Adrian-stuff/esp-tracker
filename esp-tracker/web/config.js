// Public Supabase config.
//
// Both values below are MEANT to be public — the anon key is a client
// credential and every request it makes is still filtered by RLS.
//
// That is only true if RLS is actually on. Run migrations/0002_rls.sql and
// verify in the dashboard (Table Editor → each table shows "RLS enabled")
// before this page ever sees real data. A table without RLS is readable by
// anyone who opens devtools, and this one holds a child's live location.
//
// The SERVICE ROLE key is NOT public and must never appear in this directory.
window.SUPABASE_URL = "https://nvdumsbxspevpvligzlw.supabase.co";
window.SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im52ZHVtc2J4c3BldnB2bGlnemx3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODgwOTUyNzgsImV4cCI6MjEwMzY3MTI3OH0.ejA8q5f5z4rKx-GAwC-c3fO9jDE3MhXnIh-A32vUhtA";
