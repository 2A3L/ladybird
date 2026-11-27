# Importing Edge data on Linux

The Qt browser can restore Edge favorites, history, and open tabs from a
separate Ladybird profile. Close Edge to keep the source files stable. Use
the database directory of a profile created by the current Ladybird build
as the schema template.

```sh
python3 Meta/import-edge-profile.py \
    --edge-profile "$HOME/.config/microsoft-edge/Default" \
    --schema-directory "$HOME/.local/share/Ladybird/Profiles/developer" \
    --destination "$HOME/.local/share/Ladybird/edge-import"
ladybird --profile-path "$HOME/.local/share/Ladybird/edge-import"
```

The importer checks the database schema versions and only publishes a new
profile after checking the imported databases. Use `--session` to select a
specific Edge `Session_*` file. The default uses the newest snapshot.

Favorite folders, tab order, the active tab and pinned state are preserved.
History uses one row per URL with visit counts and the last visit time.
The import covers HTTP and HTTPS URLs and translates Edge's new-tab URL.
Site logins, passwords, form state, tab groups, per-tab navigation stacks,
extensions and Edge download records require separate handling.
