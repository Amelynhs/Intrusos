-- Supabase schema for Sistema de Deteccion de Intrusos IoT
create extension if not exists pgcrypto;

create table if not exists public.devices (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  device_key text not null unique,
  status text not null default 'offline' check (status in ('online','offline','armed','intrusion')),
  last_seen_at timestamptz,
  created_at timestamptz not null default now()
);

create table if not exists public.schedules (
  id uuid primary key default gen_random_uuid(),
  device_id uuid not null references public.devices(id) on delete cascade,
  day_of_week integer not null default 0 check (day_of_week between 0 and 6),
  enabled boolean not null default false,
  start_minute integer not null check (start_minute between 0 and 1439),
  end_minute integer not null check (end_minute between 0 and 1439),
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.intrusion_events (
  id uuid primary key default gen_random_uuid(),
  device_id uuid not null references public.devices(id) on delete cascade,
  detected_at timestamptz not null default now(),
  event_type text not null default 'motion',
  status text not null default 'detected' check (status in ('detected','cleared','email_pending','email_sent','email_failed')),
  image_url text,
  metadata jsonb not null default '{}'::jsonb,
  created_at timestamptz not null default now()
);

create table if not exists public.system_logs (
  id uuid primary key default gen_random_uuid(),
  device_id uuid references public.devices(id) on delete cascade,
  event text not null,
  message text,
  created_at timestamptz not null default now()
);

create index if not exists schedules_device_day_idx on public.schedules(device_id, day_of_week);
create index if not exists intrusion_events_device_id_idx on public.intrusion_events(device_id);
create index if not exists intrusion_events_detected_at_idx on public.intrusion_events(detected_at desc);
create index if not exists system_logs_device_id_idx on public.system_logs(device_id);

insert into public.devices (id,name,device_key,status) values
('00000000-0000-0000-0000-000000000001','ESP32-S3-CAM Principal','ESP32CAM-PRINCIPAL-01','offline')
on conflict (id) do update set name=excluded.name, device_key=excluded.device_key;

create or replace function public.set_updated_at() returns trigger language plpgsql as $$
begin new.updated_at=now(); return new; end; $$;
drop trigger if exists schedules_set_updated_at on public.schedules;
create trigger schedules_set_updated_at before update on public.schedules for each row execute function public.set_updated_at();

alter table public.devices enable row level security;
alter table public.schedules enable row level security;
alter table public.intrusion_events enable row level security;
alter table public.system_logs enable row level security;

grant select on public.devices to anon, authenticated;
grant select,insert,update,delete on public.schedules to anon, authenticated;
grant select,insert,update on public.intrusion_events to anon, authenticated;
grant select,insert on public.system_logs to anon, authenticated;

drop policy if exists devices_read on public.devices;
create policy devices_read on public.devices for select to anon,authenticated using (id='00000000-0000-0000-0000-000000000001'::uuid);
drop policy if exists schedules_all on public.schedules;
create policy schedules_all on public.schedules for all to anon,authenticated using (device_id='00000000-0000-0000-0000-000000000001'::uuid) with check (device_id='00000000-0000-0000-0000-000000000001'::uuid);
drop policy if exists intrusion_all on public.intrusion_events;
create policy intrusion_all on public.intrusion_events for all to anon,authenticated using (device_id='00000000-0000-0000-0000-000000000001'::uuid) with check (device_id='00000000-0000-0000-0000-000000000001'::uuid);
drop policy if exists logs_all on public.system_logs;
create policy logs_all on public.system_logs for all to anon,authenticated using (device_id='00000000-0000-0000-0000-000000000001'::uuid) with check (device_id='00000000-0000-0000-0000-000000000001'::uuid);

insert into public.schedules(device_id,day_of_week,enabled,start_minute,end_minute)
select '00000000-0000-0000-0000-000000000001'::uuid,d,false,1320,360
from generate_series(0,6) d
where not exists (select 1 from public.schedules s where s.device_id='00000000-0000-0000-0000-000000000001'::uuid and s.day_of_week=d);
