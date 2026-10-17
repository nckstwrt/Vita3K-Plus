/*
 * Copyright 2013 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

package org.vita3k.emulator.overlay;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.drawable.BitmapDrawable;
import android.view.MotionEvent;

/**
 * Custom {@link BitmapDrawable} that is capable
 * of storing it's own ID.
 */
public final class InputOverlayDrawableButton
{
  // The legacy ID identifying what type of button this Drawable represents.
  private int mLegacyId;
  private int mControl;
  private int mTrackId;
  private int mPreviousTouchX, mPreviousTouchY;
  private int mControlPositionX, mControlPositionY;
  private int mWidth;
  private int mHeight;
  private BitmapDrawable mDefaultStateBitmap;
  private BitmapDrawable mPressedStateBitmap;
  private BitmapDrawable mThirdStateBitmap = null;
  private boolean mPressedState = false;
  private int mTouchSwitchState = 0;  // 0 = front, 1 = back, 2 = front+back
  private int mRole = 0;

  /**
   * Constructor
   *
   * @param res                {@link Resources} instance.
   * @param defaultStateBitmap {@link Bitmap} to use with the default state Drawable.
   * @param pressedStateBitmap {@link Bitmap} to use with the pressed state Drawable.
   * @param legacyId           Legacy identifier (ButtonType) for this type of button.
   * @param control            Control ID for this type of button.
   */
  public InputOverlayDrawableButton(Resources res, Bitmap defaultStateBitmap,
          Bitmap pressedStateBitmap, int legacyId, int control, int role)
  {
    mTrackId = -1;
    mDefaultStateBitmap = new BitmapDrawable(res, defaultStateBitmap);
    mPressedStateBitmap = new BitmapDrawable(res, pressedStateBitmap);
    mLegacyId = legacyId;
    mControl = control;

    mWidth = mDefaultStateBitmap.getIntrinsicWidth();
    mHeight = mDefaultStateBitmap.getIntrinsicHeight();

    mRole = role;
  }

  /**
   * Gets this InputOverlayDrawableButton's legacy button ID.
   */
  public int getLegacyId()
  {
    return mLegacyId;
  }

  public int getControl()
  {
    return mControl;
  }

  public void setTrackId(int trackId)
  {
    mTrackId = trackId;
  }

  public int getTrackId()
  {
    return mTrackId;
  }

  public int getRole() { return mRole; }

  public boolean getPressed() { return mPressedState; }

  public int getTouchSwitchState() { return mTouchSwitchState; }

  public void setThirdStateBitmap(Resources res, Bitmap bitmap)
  {
    mThirdStateBitmap = new BitmapDrawable(res, bitmap);
    mThirdStateBitmap.setBounds(mDefaultStateBitmap.getBounds());
  }

  public void onConfigureTouch(MotionEvent event)
  {
    switch (event.getAction())
    {
      case MotionEvent.ACTION_DOWN:
        mPreviousTouchX = (int) event.getX();
        mPreviousTouchY = (int) event.getY();
        break;
      case MotionEvent.ACTION_MOVE:
        mControlPositionX += (int) event.getX() - mPreviousTouchX;
        mControlPositionY += (int) event.getY() - mPreviousTouchY;
        setBounds(mControlPositionX, mControlPositionY, getWidth() + mControlPositionX,
                getHeight() + mControlPositionY);
        mPreviousTouchX = (int) event.getX();
        mPreviousTouchY = (int) event.getY();
        break;
    }
  }

  public void setPosition(int x, int y)
  {
    mControlPositionX = x;
    mControlPositionY = y;
  }

  public void draw(Canvas canvas)
  {
    getCurrentStateBitmapDrawable().draw(canvas);
  }

  private BitmapDrawable getCurrentStateBitmapDrawable()
  {
    if (mRole == InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH && mThirdStateBitmap != null)
    {
      switch (mTouchSwitchState)
      {
        case 1: return mPressedStateBitmap;
        case 2: return mThirdStateBitmap;
        default: return mDefaultStateBitmap;
      }
    }
    return mPressedState ? mPressedStateBitmap : mDefaultStateBitmap;
  }

  public void setBounds(int left, int top, int right, int bottom)
  {
    mDefaultStateBitmap.setBounds(left, top, right, bottom);
    mPressedStateBitmap.setBounds(left, top, right, bottom);
    if (mThirdStateBitmap != null)
      mThirdStateBitmap.setBounds(left, top, right, bottom);
  }

  public void setOpacity(int value)
  {
    mDefaultStateBitmap.setAlpha(value);
    mPressedStateBitmap.setAlpha(value);
    if (mThirdStateBitmap != null)
      mThirdStateBitmap.setAlpha(value);
  }

  public Rect getBounds()
  {
    return mDefaultStateBitmap.getBounds();
  }

  public int getWidth()
  {
    return mWidth;
  }

  public int getHeight()
  {
    return mHeight;
  }

  public void setPressedState(boolean isPressed)
  {
    if(mRole == InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH){
      if(isPressed){
        mTouchSwitchState = (mTouchSwitchState + 1) % (mThirdStateBitmap != null ? 3 : 2);
        mPressedState = mTouchSwitchState == 1;
      }
    } else {
      mPressedState = isPressed;
    }
  }
}
