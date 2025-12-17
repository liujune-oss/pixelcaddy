@echo off
echo Syncing with GitHub...
git add .
git commit -m "Auto-sync: %date% %time%"
git push origin main
echo.
echo Sync Complete!
pause
