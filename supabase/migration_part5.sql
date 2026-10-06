-- Parte 5: consistencia y pruebas de integracion
-- Ejecutar despues de schema.sql/migration_part3_4.sql.

-- Evita dos horarios para el mismo dispositivo y dia.
do $$
begin
  delete from public.schedules a
  using public.schedules b
  where a.device_id = b.device_id
    and a.day_of_week = b.day_of_week
    and a.id > b.id;
exception when others then
  raise;
end $$;

create unique index if not exists schedules_device_day_unique
  on public.schedules(device_id, day_of_week);

create index if not exists intrusion_events_epoch_idx
  on public.intrusion_events ((metadata->>'epoch'));

-- Mantiene el contrato de estados utilizado por el firmware.
alter table public.intrusion_events drop constraint if exists intrusion_events_status_check;
alter table public.intrusion_events
  add constraint intrusion_events_status_check
  check (status in ('detected','cleared','email_pending','email_sent','email_failed'));
